// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/mojo_bindings_controller.h"

#include "content/common/view_messages.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_view.h"
#include "content/renderer/mojo_context_state.h"
#include "gin/per_context_data.h"
#include "third_party/WebKit/public/web/WebKit.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "v8/include/v8.h"

namespace content {

namespace {

const char kMojoContextStateKey[] = "MojoContextState";

struct MojoContextStateData : public base::SupportsUserData::Data {
  scoped_ptr<MojoContextState> state;
};

}  // namespace

MojoBindingsController::MojoBindingsController(RenderFrame* render_frame)
    : RenderFrameObserver(render_frame),
      RenderFrameObserverTracker<MojoBindingsController>(render_frame) {}

MojoBindingsController::~MojoBindingsController() {
}

void MojoBindingsController::CreateContextState() {
  v8::HandleScope handle_scope(blink::mainThreadIsolate());
  blink::WebLocalFrame* frame =
      render_frame()->GetWebFrame()->toWebLocalFrame();
  v8::Local<v8::Context> context = frame->mainWorldScriptContext();
  gin::PerContextData* context_data = gin::PerContextData::From(context);
  MojoContextStateData* data = new MojoContextStateData;
  data->state.reset(new MojoContextState(frame, context));
  context_data->SetUserData(kMojoContextStateKey, data);
}

void MojoBindingsController::DestroyContextState(
    v8::Local<v8::Context> context) {
  gin::PerContextData* context_data = gin::PerContextData::From(context);
  if (!context_data)
    return;
  context_data->RemoveUserData(kMojoContextStateKey);
}

MojoContextState* MojoBindingsController::GetContextState() {
  v8::HandleScope handle_scope(blink::mainThreadIsolate());
  v8::Local<v8::Context> context =
      render_frame()->GetWebFrame()->mainWorldScriptContext();
  gin::PerContextData* context_data = gin::PerContextData::From(context);
  if (!context_data)
    return NULL;
  MojoContextStateData* context_state = static_cast<MojoContextStateData*>(
      context_data->GetUserData(kMojoContextStateKey));
  return context_state ? context_state->state.get() : NULL;
}

void MojoBindingsController::WillReleaseScriptContext(
    v8::Local<v8::Context> context,
    int world_id) {
  DestroyContextState(context);
}

void MojoBindingsController::DidFinishDocumentLoad() {
  v8::HandleScope handle_scope(blink::mainThreadIsolate());
  MojoContextState* state = GetContextState();
  if (state)
    state->Run();
}

void MojoBindingsController::DidCreateDocumentElement() {
  CreateContextState();
}

void MojoBindingsController::DidClearWindowObject() {
  // NOTE: this function may be called early on twice. From the constructor
  // mainWorldScriptContext() may trigger this to be called. If we are created
  // before the page is loaded (which is very likely), then on first load this
  // is called. In the case of the latter we may have already supplied the
  // handle to the context state so that if we destroy now the handle is
  // lost. If this is the result of the first load then the contextstate should
  // be empty and we don't need to destroy it.
  MojoContextState* state = GetContextState();
  if (state && !state->module_added())
    return;

  v8::HandleScope handle_scope(blink::mainThreadIsolate());
  DestroyContextState(render_frame()->GetWebFrame()->mainWorldScriptContext());
}

}  // namespace content
