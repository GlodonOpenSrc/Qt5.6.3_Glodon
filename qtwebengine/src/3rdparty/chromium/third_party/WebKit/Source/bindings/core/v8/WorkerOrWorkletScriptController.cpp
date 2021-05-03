/*
 * Copyright (C) 2009, 2012 Google Inc. All rights reserved.
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

#include "bindings/core/v8/WorkerOrWorkletScriptController.h"

#include "bindings/core/v8/ScriptSourceCode.h"
#include "bindings/core/v8/ScriptValue.h"
#include "bindings/core/v8/V8DedicatedWorkerGlobalScope.h"
#include "bindings/core/v8/V8ErrorHandler.h"
#include "bindings/core/v8/V8Initializer.h"
#include "bindings/core/v8/V8ObjectConstructor.h"
#include "bindings/core/v8/V8ScriptRunner.h"
#include "bindings/core/v8/V8SharedWorkerGlobalScope.h"
#include "bindings/core/v8/V8WorkerGlobalScope.h"
#include "bindings/core/v8/WrapperTypeInfo.h"
#include "core/events/ErrorEvent.h"
#include "core/frame/DOMTimer.h"
#include "core/inspector/ScriptCallStack.h"
#include "core/inspector/WorkerThreadDebugger.h"
#include "core/workers/WorkerObjectProxy.h"
#include "core/workers/WorkerOrWorkletGlobalScope.h"
#include "core/workers/WorkerThread.h"
#include "platform/heap/ThreadState.h"
#include "public/platform/Platform.h"
#include <v8.h>

namespace blink {

class WorkerOrWorkletScriptController::ExecutionState final {
    STACK_ALLOCATED();
public:
    explicit ExecutionState(WorkerOrWorkletScriptController* controller)
        : hadException(false)
        , lineNumber(0)
        , columnNumber(0)
        , m_controller(controller)
        , m_outerState(controller->m_executionState)
    {
        m_controller->m_executionState = this;
    }

    ~ExecutionState()
    {
        m_controller->m_executionState = m_outerState;
    }

    DEFINE_INLINE_TRACE()
    {
        visitor->trace(m_errorEventFromImportedScript);
        visitor->trace(m_controller);
    }

    bool hadException;
    String errorMessage;
    int lineNumber;
    int columnNumber;
    String sourceURL;
    ScriptValue exception;
    RefPtrWillBeMember<ErrorEvent> m_errorEventFromImportedScript;

    // A ExecutionState context is stack allocated by
    // WorkerOrWorkletScriptController::evaluate(), with the contoller using it
    // during script evaluation. To handle nested evaluate() uses,
    // ExecutionStates are chained together;
    // |m_outerState| keeps a pointer to the context object one level out
    // (or 0, if outermost.) Upon return from evaluate(), the
    // WorkerOrWorkletScriptController's ExecutionState is popped and the
    // previous one restored (see above dtor.)
    //
    // With Oilpan, |m_outerState| isn't traced. It'll be "up the stack"
    // and its fields will be traced when scanning the stack.
    RawPtrWillBeMember<WorkerOrWorkletScriptController> m_controller;
    ExecutionState* m_outerState;
};

PassOwnPtrWillBeRawPtr<WorkerOrWorkletScriptController> WorkerOrWorkletScriptController::create(WorkerOrWorkletGlobalScope* globalScope, v8::Isolate* isolate)
{
    return adoptPtrWillBeNoop(new WorkerOrWorkletScriptController(globalScope, isolate));
}

WorkerOrWorkletScriptController::WorkerOrWorkletScriptController(WorkerOrWorkletGlobalScope* globalScope, v8::Isolate* isolate)
    : m_globalScope(globalScope)
    , m_isolate(isolate)
    , m_executionForbidden(false)
    , m_executionScheduledToTerminate(false)
    , m_rejectedPromises(RejectedPromises::create())
    , m_executionState(0)
{
    ASSERT(isolate);
    m_world = DOMWrapperWorld::create(isolate, WorkerWorldId);
}

WorkerOrWorkletScriptController::~WorkerOrWorkletScriptController()
{
    ASSERT(!m_rejectedPromises);
}

void WorkerOrWorkletScriptController::dispose()
{
    m_rejectedPromises->dispose();
    m_rejectedPromises.release();

    m_world->dispose();

    if (isContextInitialized())
        m_scriptState->disposePerContextData();
}

bool WorkerOrWorkletScriptController::initializeContextIfNeeded()
{
    v8::HandleScope handleScope(m_isolate);

    if (isContextInitialized())
        return true;

    v8::Local<v8::Context> context = v8::Context::New(m_isolate);
    if (context.IsEmpty())
        return false;

    m_scriptState = ScriptState::create(context, m_world);

    ScriptState::Scope scope(m_scriptState.get());

    // Name new context for debugging.
    WorkerThreadDebugger::setContextDebugData(context);

    // Create a new JS object and use it as the prototype for the shadow global object.
    const WrapperTypeInfo* wrapperTypeInfo = m_globalScope->scriptWrappable()->wrapperTypeInfo();

    v8::Local<v8::Function> globalScopeConstructor = m_scriptState->perContextData()->constructorForType(wrapperTypeInfo);
    if (globalScopeConstructor.IsEmpty())
        return false;

    v8::Local<v8::Object> jsGlobalScope;
    if (!V8ObjectConstructor::newInstance(m_isolate, globalScopeConstructor).ToLocal(&jsGlobalScope)) {
        m_scriptState->disposePerContextData();
        return false;
    }

    jsGlobalScope = V8DOMWrapper::associateObjectWithWrapper(m_isolate, m_globalScope->scriptWrappable(), wrapperTypeInfo, jsGlobalScope);

    // Insert the object instance as the prototype of the shadow object.
    v8::Local<v8::Object> globalObject = v8::Local<v8::Object>::Cast(m_scriptState->context()->Global()->GetPrototype());
    return v8CallBoolean(globalObject->SetPrototype(context, jsGlobalScope));
}

ScriptValue WorkerOrWorkletScriptController::evaluate(const String& script, const String& fileName, const TextPosition& scriptStartPosition, CachedMetadataHandler* cacheHandler, V8CacheOptions v8CacheOptions)
{
    if (!initializeContextIfNeeded())
        return ScriptValue();

    ScriptState::Scope scope(m_scriptState.get());

    if (!m_disableEvalPending.isEmpty()) {
        m_scriptState->context()->AllowCodeGenerationFromStrings(false);
        m_scriptState->context()->SetErrorMessageForCodeGenerationFromStrings(v8String(m_isolate, m_disableEvalPending));
        m_disableEvalPending = String();
    }

    v8::TryCatch block(m_isolate);

    v8::Local<v8::Script> compiledScript;
    v8::MaybeLocal<v8::Value> maybeResult;
    if (v8Call(V8ScriptRunner::compileScript(script, fileName, String(), scriptStartPosition, m_isolate, cacheHandler, SharableCrossOrigin, v8CacheOptions), compiledScript, block))
        maybeResult = V8ScriptRunner::runCompiledScript(m_isolate, compiledScript, m_globalScope);

    if (!block.CanContinue()) {
        forbidExecution();
        return ScriptValue();
    }

    if (block.HasCaught()) {
        v8::Local<v8::Message> message = block.Message();
        m_executionState->hadException = true;
        m_executionState->errorMessage = toCoreString(message->Get());
        if (v8Call(message->GetLineNumber(m_scriptState->context()), m_executionState->lineNumber)
            && v8Call(message->GetStartColumn(m_scriptState->context()), m_executionState->columnNumber)) {
            ++m_executionState->columnNumber;
        } else {
            m_executionState->lineNumber = 0;
            m_executionState->columnNumber = 0;
        }

        TOSTRING_DEFAULT(V8StringResource<>, sourceURL, message->GetScriptOrigin().ResourceName(), ScriptValue());
        m_executionState->sourceURL = sourceURL;
        m_executionState->exception = ScriptValue(m_scriptState.get(), block.Exception());
        block.Reset();
    } else {
        m_executionState->hadException = false;
    }

    v8::Local<v8::Value> result;
    if (!maybeResult.ToLocal(&result) || result->IsUndefined())
        return ScriptValue();

    return ScriptValue(m_scriptState.get(), result);
}

bool WorkerOrWorkletScriptController::evaluate(const ScriptSourceCode& sourceCode, RefPtrWillBeRawPtr<ErrorEvent>* errorEvent, CachedMetadataHandler* cacheHandler, V8CacheOptions v8CacheOptions)
{
    if (isExecutionForbidden())
        return false;

    ExecutionState state(this);
    evaluate(sourceCode.source(), sourceCode.url().string(), sourceCode.startPosition(), cacheHandler, v8CacheOptions);
    if (isExecutionForbidden())
        return false;
    if (state.hadException) {
        if (errorEvent) {
            if (state.m_errorEventFromImportedScript) {
                // Propagate inner error event outwards.
                *errorEvent = state.m_errorEventFromImportedScript.release();
                return false;
            }
            if (m_globalScope->shouldSanitizeScriptError(state.sourceURL, NotSharableCrossOrigin))
                *errorEvent = ErrorEvent::createSanitizedError(m_world.get());
            else
                *errorEvent = ErrorEvent::create(state.errorMessage, state.sourceURL, state.lineNumber, state.columnNumber, m_world.get());
            V8ErrorHandler::storeExceptionOnErrorEventWrapper(m_scriptState.get(), errorEvent->get(), state.exception.v8Value(), m_scriptState->context()->Global());
        } else {
            ASSERT(!m_globalScope->shouldSanitizeScriptError(state.sourceURL, NotSharableCrossOrigin));
            RefPtrWillBeRawPtr<ErrorEvent> event = nullptr;
            if (state.m_errorEventFromImportedScript)
                event = state.m_errorEventFromImportedScript.release();
            else
                event = ErrorEvent::create(state.errorMessage, state.sourceURL, state.lineNumber, state.columnNumber, m_world.get());
            m_globalScope->reportException(event, 0, nullptr, NotSharableCrossOrigin);
        }
        return false;
    }
    return true;
}

void WorkerOrWorkletScriptController::willScheduleExecutionTermination()
{
    // The mutex provides a memory barrier to ensure that once
    // termination is scheduled, isExecutionTerminating will
    // accurately reflect that state when called from another thread.
    MutexLocker locker(m_scheduledTerminationMutex);
    m_executionScheduledToTerminate = true;
}

bool WorkerOrWorkletScriptController::isExecutionTerminating() const
{
    // See comments in willScheduleExecutionTermination regarding mutex usage.
    MutexLocker locker(m_scheduledTerminationMutex);
    return m_executionScheduledToTerminate;
}

void WorkerOrWorkletScriptController::forbidExecution()
{
    ASSERT(m_globalScope->isContextThread());
    m_executionForbidden = true;
}

bool WorkerOrWorkletScriptController::isExecutionForbidden() const
{
    ASSERT(m_globalScope->isContextThread());
    return m_executionForbidden;
}

void WorkerOrWorkletScriptController::disableEval(const String& errorMessage)
{
    m_disableEvalPending = errorMessage;
}

void WorkerOrWorkletScriptController::rethrowExceptionFromImportedScript(PassRefPtrWillBeRawPtr<ErrorEvent> errorEvent, ExceptionState& exceptionState)
{
    const String& errorMessage = errorEvent->message();
    if (m_executionState)
        m_executionState->m_errorEventFromImportedScript = errorEvent;
    exceptionState.rethrowV8Exception(V8ThrowException::createGeneralError(m_isolate, errorMessage));
}

DEFINE_TRACE(WorkerOrWorkletScriptController)
{
    visitor->trace(m_globalScope);
    visitor->trace(m_rejectedPromises);
}

} // namespace blink
