/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "public/platform/WebMediaConstraints.h"

#include "wtf/PassRefPtr.h"
#include "wtf/RefCounted.h"
#include <math.h>

namespace blink {

class WebMediaConstraintsPrivate final : public RefCounted<WebMediaConstraintsPrivate> {
public:
    static PassRefPtr<WebMediaConstraintsPrivate> create();
    static PassRefPtr<WebMediaConstraintsPrivate> create(const WebVector<WebMediaConstraint>& optional, const WebVector<WebMediaConstraint>& mandatory, const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced);
    static PassRefPtr<WebMediaConstraintsPrivate> create(const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced);

    bool isEmpty() const;
    void getOptionalConstraints(WebVector<WebMediaConstraint>&);
    void getMandatoryConstraints(WebVector<WebMediaConstraint>&);
    bool getMandatoryConstraintValue(const WebString& name, WebString& value);
    bool getOptionalConstraintValue(const WebString& name, WebString& value);
    const WebMediaTrackConstraintSet& basic() const;
    const WebVector<WebMediaTrackConstraintSet>& advanced() const;

private:
    WebMediaConstraintsPrivate(const WebVector<WebMediaConstraint>& optional, const WebVector<WebMediaConstraint>& mandatory, const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced);
    WebMediaConstraintsPrivate(const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced);

    WebVector<WebMediaConstraint> m_optional;
    WebVector<WebMediaConstraint> m_mandatory;
    WebMediaTrackConstraintSet m_basic;
    WebVector<WebMediaTrackConstraintSet> m_advanced;
};

PassRefPtr<WebMediaConstraintsPrivate> WebMediaConstraintsPrivate::create()
{
    WebMediaTrackConstraintSet basic;
    WebVector<WebMediaTrackConstraintSet> advanced;
    return adoptRef(new WebMediaConstraintsPrivate(basic, advanced));
}

PassRefPtr<WebMediaConstraintsPrivate> WebMediaConstraintsPrivate::create(const WebVector<WebMediaConstraint>& optional, const WebVector<WebMediaConstraint>& mandatory, const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
{
    return adoptRef(new WebMediaConstraintsPrivate(optional, mandatory, basic, advanced));
}

WebMediaConstraintsPrivate::WebMediaConstraintsPrivate(const WebVector<WebMediaConstraint>& optional, const WebVector<WebMediaConstraint>& mandatory, const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
    : m_optional(optional)
    , m_mandatory(mandatory)
    , m_basic(basic)
    , m_advanced(advanced)
{
}

bool WebMediaConstraintsPrivate::isEmpty() const
{
    // TODO(hta): When generating advanced constraints, make sure no empty
    // elements can be added to the m_advanced vector.
    return m_basic.isEmpty() && m_advanced.isEmpty()
        && m_optional.isEmpty() && m_mandatory.isEmpty();
}

PassRefPtr<WebMediaConstraintsPrivate> WebMediaConstraintsPrivate::create(const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
{
    return adoptRef(new WebMediaConstraintsPrivate(basic, advanced));
}

WebMediaConstraintsPrivate::WebMediaConstraintsPrivate(const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
    : m_optional()
    , m_mandatory()
    , m_basic(basic)
    , m_advanced(advanced)
{
}

void WebMediaConstraintsPrivate::getOptionalConstraints(WebVector<WebMediaConstraint>& constraints)
{
    constraints = m_optional;
}

void WebMediaConstraintsPrivate::getMandatoryConstraints(WebVector<WebMediaConstraint>& constraints)
{
    constraints = m_mandatory;
}

bool WebMediaConstraintsPrivate::getMandatoryConstraintValue(const WebString& name, WebString& value)
{
    for (size_t i = 0; i < m_mandatory.size(); ++i) {
        if (m_mandatory[i].m_name == name) {
            value = m_mandatory[i].m_value;
            return true;
        }
    }
    return false;
}

bool WebMediaConstraintsPrivate::getOptionalConstraintValue(const WebString& name, WebString& value)
{
    for (size_t i = 0; i < m_optional.size(); ++i) {
        if (m_optional[i].m_name == name) {
            value = m_optional[i].m_value;
            return true;
        }
    }
    return false;
}

const WebMediaTrackConstraintSet& WebMediaConstraintsPrivate::basic() const
{
    return m_basic;
}

const WebVector<WebMediaTrackConstraintSet>& WebMediaConstraintsPrivate::advanced() const
{
    return m_advanced;
}

// *Constraints

double DoubleConstraint::kConstraintEpsilon = 0.00001;

bool LongConstraint::matches(long value) const
{
    if (m_hasMin && value < m_min) {
        return false;
    }
    if (m_hasMax && value > m_max) {
        return false;
    }
    if (m_hasExact && value != m_exact) {
        return false;
    }
    return true;
}

bool LongConstraint::isEmpty() const
{
    return !m_hasMin && !m_hasMax && !m_hasExact && !m_hasIdeal;
}

bool DoubleConstraint::matches(double value) const
{
    if (m_hasMin && value < m_min - kConstraintEpsilon) {
        return false;
    }
    if (m_hasMax && value > m_max + kConstraintEpsilon) {
        return false;
    }
    if (m_hasExact && fabs(static_cast<double>(value) - m_exact) > kConstraintEpsilon) {
        return false;
    }
    return true;
}

bool DoubleConstraint::isEmpty() const
{
    return !m_hasMin && !m_hasMax && !m_hasExact && !m_hasIdeal;
}

bool StringConstraint::matches(WebString value) const
{
    if (m_exact.isEmpty()) {
        return true;
    }
    for (const auto& choice : m_exact) {
        if (value == choice) {
            return true;
        }
    }
    return false;
}

bool StringConstraint::isEmpty() const
{
    return m_exact.isEmpty() && m_ideal.isEmpty();
}

bool BooleanConstraint::matches(bool value) const
{
    if (m_hasExact && static_cast<bool>(m_exact) != value) {
        return false;
    }
    return true;
}

bool BooleanConstraint::isEmpty() const
{
    return !m_hasIdeal && !m_hasExact;
}

bool WebMediaTrackConstraintSet::isEmpty() const
{
    return width.isEmpty() && height.isEmpty() && aspectRatio.isEmpty()
        && frameRate.isEmpty() && facingMode.isEmpty() && volume.isEmpty()
        && sampleRate.isEmpty() && sampleSize.isEmpty()
        && echoCancellation.isEmpty() && latency.isEmpty()
        && channelCount.isEmpty() && deviceId.isEmpty() && groupId.isEmpty()
        && mediaStreamSource.isEmpty() && renderToAssociatedSink.isEmpty()
        && hotwordEnabled.isEmpty() && googEchoCancellation.isEmpty()
        && googExperimentalEchoCancellation.isEmpty()
        && googAutoGainControl.isEmpty()
        && googExperimentalAutoGainControl.isEmpty()
        && googNoiseSuppression.isEmpty()
        && googHighpassFilter.isEmpty()
        && googTypingNoiseDetection.isEmpty()
        && googExperimentalNoiseSuppression.isEmpty()
        && googBeamforming.isEmpty() && googArrayGeometry.isEmpty()
        && googAudioMirroring.isEmpty();
}

// WebMediaConstraints

void WebMediaConstraints::assign(const WebMediaConstraints& other)
{
    m_private = other.m_private;
}

void WebMediaConstraints::reset()
{
    m_private.reset();
}

bool WebMediaConstraints::isEmpty() const
{
    return m_private.isNull() || m_private->isEmpty();
}

void WebMediaConstraints::getMandatoryConstraints(WebVector<WebMediaConstraint>& constraints) const
{
    ASSERT(!isNull());
    m_private->getMandatoryConstraints(constraints);
}

void WebMediaConstraints::getOptionalConstraints(WebVector<WebMediaConstraint>& constraints) const
{
    ASSERT(!isNull());
    m_private->getOptionalConstraints(constraints);
}

bool WebMediaConstraints::getMandatoryConstraintValue(const WebString& name, WebString& value) const
{
    ASSERT(!isNull());
    return m_private->getMandatoryConstraintValue(name, value);
}

bool WebMediaConstraints::getOptionalConstraintValue(const WebString& name, WebString& value) const
{
    ASSERT(!isNull());
    return m_private->getOptionalConstraintValue(name, value);
}

void WebMediaConstraints::initialize()
{
    ASSERT(isNull());
    m_private = WebMediaConstraintsPrivate::create();
}

void WebMediaConstraints::initialize(const WebVector<WebMediaConstraint>& optional, const WebVector<WebMediaConstraint>& mandatory, const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
{
    ASSERT(isNull());
    m_private = WebMediaConstraintsPrivate::create(optional, mandatory, basic, advanced);
}

void WebMediaConstraints::initialize(const WebMediaTrackConstraintSet& basic, const WebVector<WebMediaTrackConstraintSet>& advanced)
{
    ASSERT(isNull());
    m_private = WebMediaConstraintsPrivate::create(basic, advanced);
}

const WebMediaTrackConstraintSet& WebMediaConstraints::basic() const
{
    ASSERT(!isNull());
    return m_private->basic();
}

const WebVector<WebMediaTrackConstraintSet>& WebMediaConstraints::advanced() const
{
    ASSERT(!isNull());
    return m_private->advanced();
}

} // namespace blink
