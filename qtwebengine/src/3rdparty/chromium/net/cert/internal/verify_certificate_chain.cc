// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/verify_certificate_chain.h"

#include "base/logging.h"
#include "net/cert/internal/parse_certificate.h"
#include "net/cert/internal/signature_algorithm.h"
#include "net/cert/internal/signature_policy.h"
#include "net/cert/internal/verify_name_match.h"
#include "net/cert/internal/verify_signed_data.h"
#include "net/der/input.h"
#include "net/der/parser.h"

namespace net {

namespace {

// Map from OID to ParsedExtension.
using ExtensionsMap = std::map<der::Input, ParsedExtension>;

// Describes all parsed properties of a certificate that are relevant for
// certificate verification.
struct FullyParsedCert {
  ParsedCertificate cert;
  ParsedTbsCertificate tbs;

  scoped_ptr<SignatureAlgorithm> signature_algorithm;

  // Standard extensions that were parsed.
  bool has_basic_constraints = false;
  ParsedBasicConstraints basic_constraints;

  bool has_key_usage = false;
  der::BitString key_usage;

  // The remaining extensions (excludes the standard ones above).
  ExtensionsMap unconsumed_extensions;
};

// Removes the extension with OID |oid| from |unconsumed_extensions| and fills
// |extension| with the matching extension value. If there was no extension
// matching |oid| then returns |false|.
WARN_UNUSED_RESULT bool ConsumeExtension(const der::Input& oid,
                                         ExtensionsMap* unconsumed_extensions,
                                         ParsedExtension* extension) {
  auto it = unconsumed_extensions->find(oid);
  if (it == unconsumed_extensions->end())
    return false;

  *extension = it->second;
  unconsumed_extensions->erase(it);
  return true;
}

// Returns true if the certificate does not contain any unconsumed _critical_
// extensions.
WARN_UNUSED_RESULT bool VerifyNoUnconsumedCriticalExtensions(
    const FullyParsedCert& cert) {
  for (const auto& entry : cert.unconsumed_extensions) {
    if (entry.second.critical)
      return false;
  }
  return true;
}

// Parses an X.509 Certificate fully (including the TBSCertificate and
// standard extensions), saving all the properties to |out_|.
WARN_UNUSED_RESULT bool FullyParseCertificate(const der::Input& cert_tlv,
                                              FullyParsedCert* out) {
  // Parse the outer Certificate.
  if (!ParseCertificate(cert_tlv, &out->cert))
    return false;

  // Parse the signature algorithm contained in the Certificate (there is
  // another one in the TBSCertificate, which is checked later by
  // VerifySignatureAlgorithmsMatch)
  out->signature_algorithm =
      SignatureAlgorithm::CreateFromDer(out->cert.signature_algorithm_tlv);
  if (!out->signature_algorithm)
    return false;

  // Parse the TBSCertificate.
  if (!ParseTbsCertificate(out->cert.tbs_certificate_tlv, &out->tbs))
    return false;

  // Reset state relating to extensions (which may not get overwritten). This is
  // just a precaution, since in practice |out| will already be default
  // initialize.
  out->has_basic_constraints = false;
  out->has_key_usage = false;
  out->unconsumed_extensions.clear();

  // Parse the standard X.509 extensions and remove them from
  // |unconsumed_extensions|.
  if (out->tbs.has_extensions) {
    // ParseExtensions() ensures there are no duplicates, and maps the (unique)
    // OID to the extension value.
    if (!ParseExtensions(out->tbs.extensions_tlv, &out->unconsumed_extensions))
      return false;

    ParsedExtension extension;

    // Basic constraints.
    if (ConsumeExtension(BasicConstraintsOid(), &out->unconsumed_extensions,
                         &extension)) {
      out->has_basic_constraints = true;
      if (!ParseBasicConstraints(extension.value, &out->basic_constraints))
        return false;
    }

    // KeyUsage.
    if (ConsumeExtension(KeyUsageOid(), &out->unconsumed_extensions,
                         &extension)) {
      out->has_key_usage = true;
      if (!ParseKeyUsage(extension.value, &out->key_usage))
        return false;
    }
  }

  return true;
}

WARN_UNUSED_RESULT bool GetSequenceValue(const der::Input& tlv,
                                         der::Input* value) {
  der::Parser parser(tlv);
  return parser.ReadTag(der::kSequence, value) && !parser.HasMore();
}

// Returns true if |name1_tlv| matches |name2_tlv|. The two inputs must be
// tag-length-value for RFC 5280's Name.
WARN_UNUSED_RESULT bool NameMatches(const der::Input& name1_tlv,
                                    const der::Input& name2_tlv) {
  der::Input name1_value;
  der::Input name2_value;

  // Assume that the Name is an RDNSequence. VerifyNameMatch() expects the
  // value from a SEQUENCE, so strip off the tag.
  if (!GetSequenceValue(name1_tlv, &name1_value) ||
      !GetSequenceValue(name2_tlv, &name2_value)) {
    return false;
  }

  return VerifyNameMatch(name1_value, name2_value);
}

// Returns true if |cert| was self-issued. The definition of self-issuance
// comes from RFC 5280 section 6.1:
//
//    A certificate is self-issued if the same DN appears in the subject
//    and issuer fields (the two DNs are the same if they match according
//    to the rules specified in Section 7.1).  In general, the issuer and
//    subject of the certificates that make up a path are different for
//    each certificate.  However, a CA may issue a certificate to itself to
//    support key rollover or changes in certificate policies.  These
//    self-issued certificates are not counted when evaluating path length
//    or name constraints.
WARN_UNUSED_RESULT bool IsSelfIssued(const FullyParsedCert& cert) {
  return NameMatches(cert.tbs.subject_tlv, cert.tbs.issuer_tlv);
}

// Finds a trust anchor that matches |name| in |trust_store| or returns
// nullptr. The returned pointer references data in |trust_store|.
//
// TODO(eroman): This implementation is linear in the size of the trust store,
// and also presumes that all names are unique. In practice it is possible to
// have multiple SPKIs with the same name. Also this mechanism of
// searching is fairly primitive, and does not take advantage of other
// properties like the authority key id.
WARN_UNUSED_RESULT const TrustAnchor* FindTrustAnchorByName(
    const TrustStore& trust_store,
    const der::Input& name) {
  for (const auto& anchor : trust_store.anchors) {
    if (NameMatches(name, der::Input(&anchor.name)))
      return &anchor;
  }
  return nullptr;
}

// Returns true if |cert| is valid at time |time|.
//
// The certificate's validity requirements are described by RFC 5280 section
// 4.1.2.5:
//
//    The validity period for a certificate is the period of time from
//    notBefore through notAfter, inclusive.
WARN_UNUSED_RESULT bool VerifyTimeValidity(const FullyParsedCert& cert,
                                           const der::GeneralizedTime time) {
  return !(time < cert.tbs.validity_not_before) &&
         !(cert.tbs.validity_not_after < time);
}

// Returns true if |signature_algorithm_tlv| is a valid algorithm encoding for
// RSA with SHA1.
WARN_UNUSED_RESULT bool IsRsaWithSha1SignatureAlgorithm(
    const der::Input& signature_algorithm_tlv) {
  scoped_ptr<SignatureAlgorithm> algorithm =
      SignatureAlgorithm::CreateFromDer(signature_algorithm_tlv);

  return algorithm &&
         algorithm->algorithm() == SignatureAlgorithmId::RsaPkcs1 &&
         algorithm->digest() == DigestAlgorithm::Sha1;
}

// Returns true if |cert| has internally consistent signature algorithms.
//
// X.509 certificates contain two different signature algorithms:
//  (1) The signatureAlgorithm field of Certificate
//  (2) The signature field of TBSCertificate
//
// According to RFC 5280 section 4.1.1.2 and 4.1.2.3 these two fields must be
// equal:
//
//     This field MUST contain the same algorithm identifier as the
//     signature field in the sequence tbsCertificate (Section 4.1.2.3).
//
// The spec is not explicit about what "the same algorithm identifier" means.
// Our interpretation is that the two DER-encoded fields must be byte-for-byte
// identical.
//
// In practice however there are certificates which use different encodings for
// specifying RSA with SHA1 (different OIDs). This is special-cased for
// compatibility sake.
WARN_UNUSED_RESULT bool VerifySignatureAlgorithmsMatch(
    const FullyParsedCert& cert) {
  const der::Input& alg1_tlv = cert.cert.signature_algorithm_tlv;
  const der::Input& alg2_tlv = cert.tbs.signature_algorithm_tlv;

  // Ensure that the two DER-encoded signature algorithms are byte-for-byte
  // equal, but make a compatibility concession for RSA with SHA1.
  return alg1_tlv.Equals(alg2_tlv) ||
         (IsRsaWithSha1SignatureAlgorithm(alg1_tlv) &&
          IsRsaWithSha1SignatureAlgorithm(alg2_tlv));
}

// This function corresponds to RFC 5280 section 6.1.3's "Basic Certificate
// Processing" procedure.
WARN_UNUSED_RESULT bool BasicCertificateProcessing(
    const FullyParsedCert& cert,
    const SignaturePolicy* signature_policy,
    const der::GeneralizedTime& time,
    const der::Input& working_spki,
    const der::Input& working_issuer_name) {
  // Check that the signature algorithms in Certificate vs TBSCertificate
  // match. This isn't part of RFC 5280 section 6.1.3, but is mandated by
  // sections 4.1.1.2 and 4.1.2.3.
  if (!VerifySignatureAlgorithmsMatch(cert))
    return false;

  // Verify the digital signature using the previous certificate's (or trust
  // anchor's) key (RFC 5280 section 6.1.3 step a.1).
  if (!VerifySignedData(
          *cert.signature_algorithm, cert.cert.tbs_certificate_tlv,
          cert.cert.signature_value, working_spki, signature_policy)) {
    return false;
  }

  // Check the time range for the certificate's validity, ensuring it is valid
  // at |time|.
  // (RFC 5280 section 6.1.3 step a.2)
  if (!VerifyTimeValidity(cert, time))
    return false;

  // TODO(eroman): Check revocation (RFC 5280 section 6.1.3 step a.3)

  // Verify the certificate's issuer name matches the issuing certificate's (or
  // trust anchor's) subject name. (RFC 5280 section 6.1.3 step a.4)
  if (!NameMatches(cert.tbs.issuer_tlv, working_issuer_name))
    return false;

  // TODO(eroman): Steps b-f are omitted, as policy/name constraints are not yet
  // implemented.

  return true;
}

// This function corresponds to RFC 5280 section 6.1.4's "Preparation for
// Certificate i+1" procedure. |cert| is expected to be an intermediary.
WARN_UNUSED_RESULT bool PrepareForNextCertificate(
    const FullyParsedCert& cert,
    size_t* max_path_length_ptr,
    der::Input* working_spki,
    der::Input* working_issuer_name) {
  // TODO(eroman): Steps a-b are omitted, as policy/name constraints are not yet
  // implemented.

  // From RFC 5280 section 6.1.4 step c:
  //
  //    Assign the certificate subject name to working_issuer_name.
  *working_issuer_name = cert.tbs.subject_tlv;

  // From RFC 5280 section 6.1.4 step d:
  //
  //    Assign the certificate subjectPublicKey to working_public_key.
  *working_spki = cert.tbs.spki_tlv;

  // Note that steps e and f are omitted as they are handled by
  // the assignment to |working_spki| above. See the definition
  // of |working_spki|.

  // TODO(eroman): Steps g-j are omitted as policy/name constraints are not yet
  // implemented.

  // From RFC 5280 section 6.1.4 step k:
  //
  //    If certificate i is a version 3 certificate, verify that the
  //    basicConstraints extension is present and that cA is set to
  //    TRUE.  (If certificate i is a version 1 or version 2
  //    certificate, then the application MUST either verify that
  //    certificate i is a CA certificate through out-of-band means
  //    or reject the certificate.  Conforming implementations may
  //    choose to reject all version 1 and version 2 intermediate
  //    certificates.)
  //
  // This code implicitly rejects non version 3 intermediaries, since they
  // can't contain a BasicConstraints extension.
  if (!cert.has_basic_constraints || !cert.basic_constraints.is_ca)
    return false;

  // From RFC 5280 section 6.1.4 step l:
  //
  //    If the certificate was not self-issued, verify that
  //    max_path_length is greater than zero and decrement
  //    max_path_length by 1.
  if (!IsSelfIssued(cert)) {
    if (*max_path_length_ptr == 0)
      return false;
    --(*max_path_length_ptr);
  }

  // From RFC 5280 section 6.1.4 step m:
  //
  //    If pathLenConstraint is present in the certificate and is
  //    less than max_path_length, set max_path_length to the value
  //    of pathLenConstraint.
  if (cert.basic_constraints.has_path_len &&
      cert.basic_constraints.path_len < *max_path_length_ptr) {
    *max_path_length_ptr = cert.basic_constraints.path_len;
  }

  // From RFC 5280 section 6.1.4 step n:
  //
  //    If a key usage extension is present, verify that the
  //    keyCertSign bit is set.
  if (cert.has_key_usage &&
      !cert.key_usage.AssertsBit(KEY_USAGE_BIT_KEY_CERT_SIGN)) {
    return false;
  }

  // From RFC 5280 section 6.1.4 step o:
  //
  //    Recognize and process any other critical extension present in
  //    the certificate.  Process any other recognized non-critical
  //    extension present in the certificate that is relevant to path
  //    processing.
  if (!VerifyNoUnconsumedCriticalExtensions(cert))
    return false;

  return true;
}

// Checks that if the target certificate has properties that only a CA should
// have (keyCertSign, CA=true, pathLenConstraint), then its other properties
// are consistent with being a CA.
//
// This follows from some requirements in RFC 5280 section 4.2.1.9. In
// particular:
//
//    CAs MUST NOT include the pathLenConstraint field unless the cA
//    boolean is asserted and the key usage extension asserts the
//    keyCertSign bit.
//
// And:
//
//    If the cA boolean is not asserted, then the keyCertSign bit in the key
//    usage extension MUST NOT be asserted.
//
// TODO(eroman): Strictly speaking the first requirement is on CAs and not the
// certificate client, so could be skipped.
//
// TODO(eroman): I don't believe Firefox enforces the keyCertSign restriction
// for compatibility reasons. Investigate if we need to similarly relax this
// constraint.
WARN_UNUSED_RESULT bool VerifyTargetCertHasConsistentCaBits(
    const FullyParsedCert& cert) {
  // Check if the certificate contains any property specific to CAs.
  bool has_ca_property =
      (cert.has_basic_constraints &&
       (cert.basic_constraints.is_ca || cert.basic_constraints.has_path_len)) ||
      (cert.has_key_usage &&
       cert.key_usage.AssertsBit(KEY_USAGE_BIT_KEY_CERT_SIGN));

  // If it "looks" like a CA because it has a CA-only property, then check that
  // it sets ALL the properties expected of a CA.
  if (has_ca_property) {
    return cert.has_basic_constraints && cert.basic_constraints.is_ca &&
           (!cert.has_key_usage ||
            cert.key_usage.AssertsBit(KEY_USAGE_BIT_KEY_CERT_SIGN));
  }

  return true;
}

// This function corresponds with RFC 5280 section 6.1.5's "Wrap-Up Procedure".
// It does processing for the final certificate (the target cert).
WARN_UNUSED_RESULT bool WrapUp(const FullyParsedCert& cert) {
  // TODO(eroman): Steps a-c are omitted as policy/name constraints are not yet
  // implemented.

  // Note step c-e are omitted the verification function does
  // not output the working public key.

  // From RFC 5280 section 6.1.5 step f:
  //
  //    Recognize and process any other critical extension present in
  //    the certificate n.  Process any other recognized non-critical
  //    extension present in certificate n that is relevant to path
  //    processing.
  //
  // Note that this is duplicated by PrepareForNextCertificate() so as to
  // directly match the procedures in RFC 5280's section 6.1.
  if (!VerifyNoUnconsumedCriticalExtensions(cert))
    return false;

  // TODO(eroman): Step g is omitted, as policy constraints are not yet
  // implemented.

  // The following check is NOT part of RFC 5280 6.1.5's "Wrap-Up Procedure",
  // however is implied by RFC 5280 section 4.2.1.9.
  if (!VerifyTargetCertHasConsistentCaBits(cert))
    return false;

  return true;
}

}  // namespace

TrustAnchor::~TrustAnchor() {}

TrustStore::TrustStore() {}
TrustStore::~TrustStore() {}

// This implementation is structured to mimic the description of certificate
// path verification given by RFC 5280 section 6.1.
bool VerifyCertificateChain(const std::vector<der::Input>& certs_der,
                            const TrustStore& trust_store,
                            const SignaturePolicy* signature_policy,
                            const der::GeneralizedTime& time) {
  // An empty chain is necessarily invalid.
  if (certs_der.empty())
    return false;

  // |working_spki| is an amalgamation of 3 separate variables from RFC 5280:
  //    * working_public_key
  //    * working_public_key_algorithm
  //    * working_public_key_parameters
  //
  // They are combined for simplicity since the signature verification takes an
  // SPKI, and the parameter inheritence is not applicable for the supported
  // key types.
  //
  // An approximate explanation of |working_spki| is this description from RFC
  // 5280 section 6.1.2:
  //
  //    working_public_key:  the public key used to verify the
  //    signature of a certificate.  The working_public_key is
  //    initialized from the trusted public key provided in the trust
  //    anchor information.
  der::Input working_spki;

  // |working_issuer_name| corresponds with the same named variable in RFC 5280
  // section 6.1.2:
  //
  //    working_issuer_name:  the issuer distinguished name expected
  //    in the next certificate in the chain.  The
  //    working_issuer_name is initialized to the trusted issuer name
  //    provided in the trust anchor information.
  der::Input working_issuer_name;

  // |max_path_length| corresponds with the same named variable in RFC 5280
  // section 6.1.2:
  //
  //    max_path_length:  this integer is initialized to n, is
  //    decremented for each non-self-issued certificate in the path,
  //    and may be reduced to the value in the path length constraint
  //    field within the basic constraints extension of a CA
  //    certificate.
  size_t max_path_length = certs_der.size();

  // Iterate over all the certificates in the reverse direction: starting from
  // the trust anchor and progressing towards the target certificate.
  //
  // Note that |i| uses 0-based indexing whereas in RFC 5280 it is 1-based.
  //
  //   * i=0    :  Certificate signed by a trust anchor.
  //   * i=N-1  :  Target certificate.
  for (size_t i = 0; i < certs_der.size(); ++i) {
    const size_t index_into_certs_der = certs_der.size() - i - 1;
    const bool is_target_cert = index_into_certs_der == 0;

    // Parse the current certificate into |cert|.
    FullyParsedCert cert;
    const der::Input& cert_der = certs_der[index_into_certs_der];
    if (!FullyParseCertificate(cert_der, &cert))
      return false;

    // When processing the first certificate, initialize |working_spki|
    // and |working_issuer_name| to the trust anchor per RFC 5280 section 6.1.2.
    // This is done inside the loop in order to have access to the parsed
    // certificate.
    if (i == 0) {
      const TrustAnchor* trust_anchor =
          FindTrustAnchorByName(trust_store, cert.tbs.issuer_tlv);
      if (!trust_anchor)
        return false;
      working_spki = der::Input(&trust_anchor->spki);
      working_issuer_name = der::Input(&trust_anchor->name);
    }

    // Per RFC 5280 section 6.1:
    //  * Do basic processing for each certificate
    //  * If it is the last certificate in the path (target certificate)
    //     - Then run "Wrap up"
    //     - Otherwise run "Prepare for Next cert"
    if (!BasicCertificateProcessing(cert, signature_policy, time, working_spki,
                                    working_issuer_name)) {
      return false;
    }
    if (!is_target_cert) {
      if (!PrepareForNextCertificate(cert, &max_path_length, &working_spki,
                                     &working_issuer_name)) {
        return false;
      }
    } else {
      if (!WrapUp(cert))
        return false;
    }
  }

  // TODO(eroman): RFC 5280 forbids duplicate certificates per section 6.1:
  //
  //    A certificate MUST NOT appear more than once in a prospective
  //    certification path.

  return true;
}

}  // namespace net
