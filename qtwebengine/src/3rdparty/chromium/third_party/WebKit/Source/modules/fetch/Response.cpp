// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "modules/fetch/Response.h"

#include "bindings/core/v8/Dictionary.h"
#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/ReadableStreamOperations.h"
#include "bindings/core/v8/ScriptState.h"
#include "bindings/core/v8/V8ArrayBuffer.h"
#include "bindings/core/v8/V8ArrayBufferView.h"
#include "bindings/core/v8/V8Binding.h"
#include "bindings/core/v8/V8Blob.h"
#include "bindings/core/v8/V8FormData.h"
#include "core/dom/DOMArrayBuffer.h"
#include "core/dom/DOMArrayBufferView.h"
#include "core/fileapi/Blob.h"
#include "core/html/FormData.h"
#include "modules/fetch/BodyStreamBuffer.h"
#include "modules/fetch/FetchBlobDataConsumerHandle.h"
#include "modules/fetch/FetchFormDataConsumerHandle.h"
#include "modules/fetch/ReadableStreamDataConsumerHandle.h"
#include "modules/fetch/ResponseInit.h"
#include "platform/RuntimeEnabledFeatures.h"
#include "platform/network/EncodedFormData.h"
#include "platform/network/HTTPHeaderMap.h"
#include "public/platform/modules/serviceworker/WebServiceWorkerResponse.h"
#include "wtf/RefPtr.h"

namespace blink {

namespace {

FetchResponseData* createFetchResponseDataFromWebResponse(ExecutionContext* executionContext, const WebServiceWorkerResponse& webResponse)
{
    FetchResponseData* response = nullptr;
    if (webResponse.status() > 0)
        response = FetchResponseData::create();
    else
        response = FetchResponseData::createNetworkErrorResponse();

    response->setURL(webResponse.url());
    response->setStatus(webResponse.status());
    response->setStatusMessage(webResponse.statusText());

    for (HTTPHeaderMap::const_iterator i = webResponse.headers().begin(), end = webResponse.headers().end(); i != end; ++i) {
        response->headerList()->append(i->key, i->value);
    }

    response->replaceBodyStreamBuffer(new BodyStreamBuffer(FetchBlobDataConsumerHandle::create(executionContext, webResponse.blobDataHandle())));

    // Filter the response according to |webResponse|'s ResponseType.
    switch (webResponse.responseType()) {
    case WebServiceWorkerResponseTypeBasic:
        response = response->createBasicFilteredResponse();
        break;
    case WebServiceWorkerResponseTypeCORS:
        response = response->createCORSFilteredResponse();
        break;
    case WebServiceWorkerResponseTypeOpaque:
        response = response->createOpaqueFilteredResponse();
        break;
    case WebServiceWorkerResponseTypeOpaqueRedirect:
        response = response->createOpaqueRedirectFilteredResponse();
        break;
    case WebServiceWorkerResponseTypeDefault:
        break;
    case WebServiceWorkerResponseTypeError:
        ASSERT(response->type() == FetchResponseData::ErrorType);
        break;
    }

    return response;
}

// Checks whether |status| is a null body status.
// Spec: https://fetch.spec.whatwg.org/#null-body-status
bool isNullBodyStatus(unsigned short status)
{
    if (status == 101 || status == 204 || status == 205 || status == 304)
        return true;

    return false;
}

// Check whether |statusText| is a ByteString and
// matches the Reason-Phrase token production.
// RFC 2616: https://tools.ietf.org/html/rfc2616
// RFC 7230: https://tools.ietf.org/html/rfc7230
// "reason-phrase = *( HTAB / SP / VCHAR / obs-text )"
bool isValidReasonPhrase(const String& statusText)
{
    for (unsigned i = 0; i < statusText.length(); ++i) {
        UChar c = statusText[i];
        if (!(c == 0x09 // HTAB
            || (0x20 <= c && c <= 0x7E) // SP / VCHAR
            || (0x80 <= c && c <= 0xFF))) // obs-text
            return false;
    }
    return true;
}

}

Response* Response::create(ScriptState* scriptState, ExceptionState& exceptionState)
{
    return create(scriptState->executionContext(), nullptr, String(), ResponseInit(), exceptionState);
}

Response* Response::create(ScriptState* scriptState, ScriptValue bodyValue, const Dictionary& init, ExceptionState& exceptionState)
{
    v8::Local<v8::Value> body = bodyValue.v8Value();
    v8::Isolate* isolate = scriptState->isolate();
    ExecutionContext* executionContext = scriptState->executionContext();

    OwnPtr<FetchDataConsumerHandle> bodyHandle;
    String contentType;
    if (bodyValue.isUndefined() || bodyValue.isNull()) {
        // Note: The IDL processor cannot handle this situation. See
        // https://crbug.com/335871.
    } else if (V8Blob::hasInstance(body, isolate)) {
        Blob* blob = V8Blob::toImpl(body.As<v8::Object>());
        bodyHandle = FetchBlobDataConsumerHandle::create(executionContext, blob->blobDataHandle());
        contentType = blob->type();
    } else if (V8ArrayBuffer::hasInstance(body, isolate)) {
        bodyHandle = FetchFormDataConsumerHandle::create(V8ArrayBuffer::toImpl(body.As<v8::Object>()));
    } else if (V8ArrayBufferView::hasInstance(body, isolate)) {
        bodyHandle = FetchFormDataConsumerHandle::create(V8ArrayBufferView::toImpl(body.As<v8::Object>()));
    } else if (V8FormData::hasInstance(body, isolate)) {
        RefPtr<EncodedFormData> formData = V8FormData::toImpl(body.As<v8::Object>())->encodeMultiPartFormData();
        // Here we handle formData->boundary() as a C-style string. See
        // FormDataEncoder::generateUniqueBoundaryString.
        contentType = AtomicString("multipart/form-data; boundary=", AtomicString::ConstructFromLiteral) + formData->boundary().data();
        bodyHandle = FetchFormDataConsumerHandle::create(executionContext, formData.release());
    } else if (RuntimeEnabledFeatures::responseConstructedWithReadableStreamEnabled() && ReadableStreamOperations::isReadableStream(scriptState, body)) {
        bodyHandle = ReadableStreamDataConsumerHandle::create(scriptState, body);
    } else {
        String string = toUSVString(isolate, body, exceptionState);
        if (exceptionState.hadException())
            return nullptr;
        bodyHandle = FetchFormDataConsumerHandle::create(string);
        contentType = "text/plain;charset=UTF-8";
    }
    // TODO(yhirano): Add the URLSearchParams case.
    return create(executionContext, bodyHandle.release(), contentType, ResponseInit(init, exceptionState), exceptionState);
}

Response* Response::create(ExecutionContext* context, PassOwnPtr<FetchDataConsumerHandle> bodyHandle, const String& contentType, const ResponseInit& init, ExceptionState& exceptionState)
{
    unsigned short status = init.status;

    // "1. If |init|'s status member is not in the range 200 to 599, inclusive, throw a
    // RangeError."
    if (status < 200 || 599 < status) {
        exceptionState.throwRangeError(ExceptionMessages::indexOutsideRange<unsigned>("status", status, 200, ExceptionMessages::InclusiveBound, 599, ExceptionMessages::InclusiveBound));
        return nullptr;
    }

    // "2. If |init|'s statusText member does not match the Reason-Phrase
    // token production, throw a TypeError."
    if (!isValidReasonPhrase(init.statusText)) {
        exceptionState.throwTypeError("Invalid statusText");
        return nullptr;
    }

    // "3. Let |r| be a new Response object, associated with a new response,
    // Headers object, and Body object."
    Response* r = new Response(context);

    // "4. Set |r|'s response's status to |init|'s status member."
    r->m_response->setStatus(init.status);

    // "5. Set |r|'s response's status message to |init|'s statusText member."
    r->m_response->setStatusMessage(AtomicString(init.statusText));

    // "6. If |init|'s headers member is present, run these substeps:"
    if (init.headers) {
        // "1. Empty |r|'s response's header list."
        r->m_response->headerList()->clearList();
        // "2. Fill |r|'s Headers object with |init|'s headers member. Rethrow
        // any exceptions."
        r->m_headers->fillWith(init.headers.get(), exceptionState);
        if (exceptionState.hadException())
            return nullptr;
    } else if (!init.headersDictionary.isUndefinedOrNull()) {
        // "1. Empty |r|'s response's header list."
        r->m_response->headerList()->clearList();
        // "2. Fill |r|'s Headers object with |init|'s headers member. Rethrow
        // any exceptions."
        r->m_headers->fillWith(init.headersDictionary, exceptionState);
        if (exceptionState.hadException())
            return nullptr;
    }
    // "7. If body is given, run these substeps:"
    if (bodyHandle) {
        // "1. If |init|'s status member is a null body status, throw a
        //     TypeError."
        // "2. Let |stream| and |Content-Type| be the result of extracting
        //     body."
        // "3. Set |r|'s response's body to |stream|."
        // "4. If |Content-Type| is non-null and |r|'s response's header list
        // contains no header named `Content-Type`, append `Content-Type`/
        // |Content-Type| to |r|'s response's header list."
        // https://fetch.spec.whatwg.org/#concept-bodyinit-extract
        // Step 3, Blob:
        // "If object's type attribute is not the empty byte sequence, set
        // Content-Type to its value."
        if (isNullBodyStatus(status)) {
            exceptionState.throwTypeError("Response with null body status cannot have body");
            return nullptr;
        }
        r->m_response->replaceBodyStreamBuffer(new BodyStreamBuffer(bodyHandle));
        if (!contentType.isEmpty() && !r->m_response->headerList()->has("Content-Type"))
            r->m_response->headerList()->append("Content-Type", contentType);
    }

    // "8. Set |r|'s MIME type to the result of extracting a MIME type
    // from |r|'s response's header list."
    r->m_response->setMIMEType(r->m_response->headerList()->extractMIMEType());

    // "9. Return |r|."
    return r;
}

Response* Response::create(ExecutionContext* context, FetchResponseData* response)
{
    return new Response(context, response);
}

Response* Response::create(ExecutionContext* context, const WebServiceWorkerResponse& webResponse)
{
    FetchResponseData* responseData = createFetchResponseDataFromWebResponse(context, webResponse);
    return new Response(context, responseData);
}

Response* Response::error(ExecutionContext* context)
{
    FetchResponseData* responseData = FetchResponseData::createNetworkErrorResponse();
    Response* r = new Response(context, responseData);
    r->m_headers->setGuard(Headers::ImmutableGuard);
    return r;
}

Response* Response::redirect(ExecutionContext* context, const String& url, unsigned short status, ExceptionState& exceptionState)
{
    KURL parsedURL = context->completeURL(url);
    if (!parsedURL.isValid()) {
        exceptionState.throwTypeError("Failed to parse URL from " + url);
        return nullptr;
    }

    if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
        exceptionState.throwRangeError("Invalid status code");
        return nullptr;
    }

    Response* r = new Response(context);
    r->m_headers->setGuard(Headers::ImmutableGuard);
    r->m_response->setStatus(status);
    r->m_response->headerList()->set("Location", parsedURL);

    return r;
}

String Response::type() const
{
    // "The type attribute's getter must return response's type."
    switch (m_response->type()) {
    case FetchResponseData::BasicType:
        return "basic";
    case FetchResponseData::CORSType:
        return "cors";
    case FetchResponseData::DefaultType:
        return "default";
    case FetchResponseData::ErrorType:
        return "error";
    case FetchResponseData::OpaqueType:
        return "opaque";
    case FetchResponseData::OpaqueRedirectType:
        return "opaqueredirect";
    }
    ASSERT_NOT_REACHED();
    return "";
}

String Response::url() const
{
    // "The url attribute's getter must return the empty string if response's
    // url is null and response's url, serialized with the exclude fragment
    // flag set, otherwise."
    if (!m_response->url().hasFragmentIdentifier())
        return m_response->url();
    KURL url(m_response->url());
    url.removeFragmentIdentifier();
    return url;
}

unsigned short Response::status() const
{
    // "The status attribute's getter must return response's status."
    return m_response->status();
}

bool Response::ok() const
{
    // "The ok attribute's getter must return true
    // if response's status is in the range 200 to 299, and false otherwise."
    return 200 <= status() && status() <= 299;
}

String Response::statusText() const
{
    // "The statusText attribute's getter must return response's status message."
    return m_response->statusMessage();
}

Headers* Response::headers() const
{
    // "The headers attribute's getter must return the associated Headers object."
    return m_headers;
}

Response* Response::clone(ExceptionState& exceptionState)
{
    if (isBodyLocked() || bodyUsed()) {
        exceptionState.throwTypeError("Response body is already used");
        return nullptr;
    }

    FetchResponseData* response = m_response->clone(executionContext());
    Headers* headers = Headers::create(response->headerList());
    headers->setGuard(m_headers->guard());
    return new Response(executionContext(), response, headers);
}

bool Response::hasPendingActivity() const
{
    if (!executionContext() || executionContext()->activeDOMObjectsAreStopped())
        return false;
    if (!internalBodyBuffer())
        return false;
    if (internalBodyBuffer()->hasPendingActivity())
        return true;
    return Body::hasPendingActivity();
}

void Response::stop()
{
    if (m_response->internalBuffer())
        m_response->internalBuffer()->stop();
}

void Response::populateWebServiceWorkerResponse(WebServiceWorkerResponse& response)
{
    m_response->populateWebServiceWorkerResponse(response);
}

Response::Response(ExecutionContext* context)
    : Body(context)
    , m_response(FetchResponseData::create())
    , m_headers(Headers::create(m_response->headerList()))
{
    m_headers->setGuard(Headers::ResponseGuard);
}

Response::Response(ExecutionContext* context, FetchResponseData* response)
    : Body(context)
    , m_response(response)
    , m_headers(Headers::create(m_response->headerList()))
{
    m_headers->setGuard(Headers::ResponseGuard);
}

Response::Response(ExecutionContext* context, FetchResponseData* response, Headers* headers)
    : Body(context) , m_response(response) , m_headers(headers) {}

bool Response::hasBody() const
{
    return m_response->internalBuffer();
}

bool Response::bodyUsed()
{
    return internalBodyBuffer() && internalBodyBuffer()->stream()->isDisturbed();
}

String Response::mimeType() const
{
    return m_response->mimeType();
}

String Response::internalMIMEType() const
{
    return m_response->internalMIMEType();
}

DEFINE_TRACE(Response)
{
    Body::trace(visitor);
    visitor->trace(m_response);
    visitor->trace(m_headers);
}

} // namespace blink
