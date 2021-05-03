// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api.h"
#include "src/api-natives.h"
#include "src/assert-scope.h"
#include "src/ast/ast.h"
#include "src/ast/scopes.h"
#include "src/factory.h"
#include "src/handles.h"
#include "src/isolate.h"
#include "src/objects.h"
#include "src/parsing/parser.h"
#include "src/typing-asm.h"

#include "src/wasm/asm-wasm-builder.h"
#include "src/wasm/encoder.h"
#include "src/wasm/module-decoder.h"
#include "src/wasm/wasm-js.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-result.h"

typedef uint8_t byte;

using v8::internal::wasm::ErrorThrower;

namespace v8 {

namespace {
struct RawBuffer {
  const byte* start;
  const byte* end;
  size_t size() { return static_cast<size_t>(end - start); }
};


RawBuffer GetRawBufferArgument(
    ErrorThrower& thrower, const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 1 || !args[0]->IsArrayBuffer()) {
    thrower.Error("Argument 0 must be an array buffer");
    return {nullptr, nullptr};
  }
  Local<ArrayBuffer> buffer = Local<ArrayBuffer>::Cast(args[0]);
  ArrayBuffer::Contents contents = buffer->GetContents();

  // TODO(titzer): allow offsets into buffers, views, etc.

  const byte* start = reinterpret_cast<const byte*>(contents.Data());
  const byte* end = start + contents.ByteLength();

  if (start == nullptr) {
    thrower.Error("ArrayBuffer argument is empty");
  }
  return {start, end};
}


void VerifyModule(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.verifyModule()");

  RawBuffer buffer = GetRawBufferArgument(thrower, args);
  if (thrower.error()) return;

  i::Zone zone;
  internal::wasm::ModuleResult result = internal::wasm::DecodeWasmModule(
      isolate, &zone, buffer.start, buffer.end, true, false);

  if (result.failed()) {
    thrower.Failed("", result);
  }

  if (result.val) delete result.val;
}


void VerifyFunction(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.verifyFunction()");

  RawBuffer buffer = GetRawBufferArgument(thrower, args);
  if (thrower.error()) return;

  internal::wasm::FunctionResult result;
  {
    // Verification of a single function shouldn't allocate.
    i::DisallowHeapAllocation no_allocation;
    i::Zone zone;
    result = internal::wasm::DecodeWasmFunction(isolate, &zone, nullptr,
                                                buffer.start, buffer.end);
  }

  if (result.failed()) {
    thrower.Failed("", result);
  }

  if (result.val) delete result.val;
}


void CompileRun(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.compileRun()");

  RawBuffer buffer = GetRawBufferArgument(thrower, args);
  if (thrower.error()) return;

  // Decode and pre-verify the functions before compiling and running.
  i::Zone zone;
  internal::wasm::ModuleResult result = internal::wasm::DecodeWasmModule(
      isolate, &zone, buffer.start, buffer.end, true, false);

  if (result.failed()) {
    thrower.Failed("", result);
  } else {
    // Success. Compile and run!
    int32_t retval = i::wasm::CompileAndRunWasmModule(isolate, result.val);
    args.GetReturnValue().Set(retval);
  }

  if (result.val) delete result.val;
}


v8::internal::wasm::WasmModuleIndex* TranslateAsmModule(i::ParseInfo* info) {
  info->set_global();
  info->set_lazy(false);
  info->set_allow_lazy_parsing(false);
  info->set_toplevel(true);

  if (!i::Compiler::ParseAndAnalyze(info)) {
    return nullptr;
  }

  info->set_literal(
      info->scope()->declarations()->at(0)->AsFunctionDeclaration()->fun());

  v8::internal::AsmTyper typer(info->isolate(), info->zone(), *(info->script()),
                               info->literal());
  if (!typer.Validate()) {
    return nullptr;
  }

  auto module = v8::internal::wasm::AsmWasmBuilder(
                    info->isolate(), info->zone(), info->literal())
                    .Run();
  return module;
}


void AsmCompileRun(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.asmCompileRun()");

  if (args.Length() != 1) {
    thrower.Error("Invalid argument count");
    return;
  }
  if (!args[0]->IsString()) {
    thrower.Error("Invalid argument count");
    return;
  }

  i::Factory* factory = isolate->factory();
  i::Zone zone;
  Local<String> source = Local<String>::Cast(args[0]);
  i::Handle<i::Script> script = factory->NewScript(Utils::OpenHandle(*source));
  i::ParseInfo info(&zone, script);

  auto module = TranslateAsmModule(&info);
  if (module == nullptr) {
    thrower.Error("Asm.js validation failed");
    return;
  }

  int32_t result = v8::internal::wasm::CompileAndRunWasmModule(
      isolate, module->Begin(), module->End(), true);
  args.GetReturnValue().Set(result);
}


// TODO(aseemgarg): deal with arraybuffer and foreign functions
void InstantiateModuleFromAsm(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.instantiateModuleFromAsm()");

  if (args.Length() != 1) {
    thrower.Error("Invalid argument count");
    return;
  }
  if (!args[0]->IsString()) {
    thrower.Error("Invalid argument count");
    return;
  }

  i::Factory* factory = isolate->factory();
  i::Zone zone;
  Local<String> source = Local<String>::Cast(args[0]);
  i::Handle<i::Script> script = factory->NewScript(Utils::OpenHandle(*source));
  i::ParseInfo info(&zone, script);

  auto module = TranslateAsmModule(&info);
  if (module == nullptr) {
    thrower.Error("Asm.js validation failed");
    return;
  }

  i::Handle<i::JSArrayBuffer> memory = i::Handle<i::JSArrayBuffer>::null();
  internal::wasm::ModuleResult result = internal::wasm::DecodeWasmModule(
      isolate, &zone, module->Begin(), module->End(), false, false);

  if (result.failed()) {
    thrower.Failed("", result);
  } else {
    // Success. Instantiate the module and return the object.
    i::Handle<i::JSObject> ffi = i::Handle<i::JSObject>::null();

    i::MaybeHandle<i::JSObject> object =
        result.val->Instantiate(isolate, ffi, memory);

    if (!object.is_null()) {
      args.GetReturnValue().Set(v8::Utils::ToLocal(object.ToHandleChecked()));
    }
  }

  if (result.val) delete result.val;
}


void InstantiateModule(const v8::FunctionCallbackInfo<v8::Value>& args) {
  HandleScope scope(args.GetIsolate());
  i::Isolate* isolate = reinterpret_cast<i::Isolate*>(args.GetIsolate());
  ErrorThrower thrower(isolate, "WASM.instantiateModule()");

  RawBuffer buffer = GetRawBufferArgument(thrower, args);
  if (buffer.start == nullptr) return;

  i::Handle<i::JSArrayBuffer> memory = i::Handle<i::JSArrayBuffer>::null();
  if (args.Length() > 2 && args[2]->IsArrayBuffer()) {
    Local<Object> obj = Local<Object>::Cast(args[2]);
    i::Handle<i::Object> mem_obj = v8::Utils::OpenHandle(*obj);
    memory = i::Handle<i::JSArrayBuffer>(i::JSArrayBuffer::cast(*mem_obj));
  }

  // Decode but avoid a redundant pass over function bodies for verification.
  // Verification will happen during compilation.
  i::Zone zone;
  internal::wasm::ModuleResult result = internal::wasm::DecodeWasmModule(
      isolate, &zone, buffer.start, buffer.end, false, false);

  if (result.failed()) {
    thrower.Failed("", result);
  } else {
    // Success. Instantiate the module and return the object.
    i::Handle<i::JSObject> ffi = i::Handle<i::JSObject>::null();
    if (args.Length() > 1 && args[1]->IsObject()) {
      Local<Object> obj = Local<Object>::Cast(args[1]);
      ffi = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*obj));
    }

    i::MaybeHandle<i::JSObject> object =
        result.val->Instantiate(isolate, ffi, memory);

    if (!object.is_null()) {
      args.GetReturnValue().Set(v8::Utils::ToLocal(object.ToHandleChecked()));
    }
  }

  if (result.val) delete result.val;
}
}  // namespace


// TODO(titzer): we use the API to create the function template because the
// internal guts are too ugly to replicate here.
static i::Handle<i::FunctionTemplateInfo> NewTemplate(i::Isolate* i_isolate,
                                                      FunctionCallback func) {
  Isolate* isolate = reinterpret_cast<Isolate*>(i_isolate);
  Local<FunctionTemplate> local = FunctionTemplate::New(isolate, func);
  return v8::Utils::OpenHandle(*local);
}


namespace internal {
static Handle<String> v8_str(Isolate* isolate, const char* str) {
  return isolate->factory()->NewStringFromAsciiChecked(str);
}


static void InstallFunc(Isolate* isolate, Handle<JSObject> object,
                        const char* str, FunctionCallback func) {
  Handle<String> name = v8_str(isolate, str);
  Handle<FunctionTemplateInfo> temp = NewTemplate(isolate, func);
  Handle<JSFunction> function =
      ApiNatives::InstantiateFunction(temp).ToHandleChecked();
  PropertyAttributes attributes =
      static_cast<PropertyAttributes>(DONT_DELETE | READ_ONLY);
  JSObject::AddProperty(object, name, function, attributes);
}


void WasmJs::Install(Isolate* isolate, Handle<JSGlobalObject> global) {
  // Setup wasm function map.
  Handle<Context> context(global->native_context(), isolate);
  InstallWasmFunctionMap(isolate, context);

  // Bind the WASM object.
  Factory* factory = isolate->factory();
  Handle<String> name = v8_str(isolate, "_WASMEXP_");
  Handle<JSFunction> cons = factory->NewFunction(name);
  JSFunction::SetInstancePrototype(
      cons, Handle<Object>(context->initial_object_prototype(), isolate));
  cons->shared()->set_instance_class_name(*name);
  Handle<JSObject> wasm_object = factory->NewJSObject(cons, TENURED);
  PropertyAttributes attributes = static_cast<PropertyAttributes>(DONT_ENUM);
  JSObject::AddProperty(global, name, wasm_object, attributes);

  // Install functions on the WASM object.
  InstallFunc(isolate, wasm_object, "instantiateModule", InstantiateModule);
  InstallFunc(isolate, wasm_object, "verifyModule", VerifyModule);
  InstallFunc(isolate, wasm_object, "verifyFunction", VerifyFunction);
  InstallFunc(isolate, wasm_object, "compileRun", CompileRun);
  InstallFunc(isolate, wasm_object, "asmCompileRun", AsmCompileRun);
  InstallFunc(isolate, wasm_object, "instantiateModuleFromAsm",
              InstantiateModuleFromAsm);
}


void WasmJs::InstallWasmFunctionMap(Isolate* isolate, Handle<Context> context) {
  if (!context->get(Context::WASM_FUNCTION_MAP_INDEX)->IsMap()) {
    Handle<Map> wasm_function_map = isolate->factory()->NewMap(
        JS_FUNCTION_TYPE, JSFunction::kSize + kPointerSize);
    wasm_function_map->set_is_callable();
    context->set_wasm_function_map(*wasm_function_map);
  }
}

}  // namespace internal
}  // namespace v8
