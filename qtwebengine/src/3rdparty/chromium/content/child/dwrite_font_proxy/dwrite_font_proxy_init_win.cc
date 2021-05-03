// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/dwrite_font_proxy/dwrite_font_proxy_init_win.h"

#include <dwrite.h>

#include "base/bind.h"
#include "base/callback.h"
#include "base/debug/alias.h"
#include "base/win/iat_patch_function.h"
#include "base/win/windows_version.h"
#include "content/child/dwrite_font_proxy/dwrite_font_proxy_win.h"
#include "content/common/font_warmup_win.h"
#include "skia/ext/fontmgr_default_win.h"
#include "third_party/WebKit/public/web/win/WebFontRendering.h"
#include "third_party/skia/include/ports/SkTypeface_win.h"

namespace mswr = Microsoft::WRL;

namespace content {

namespace {

mswr::ComPtr<DWriteFontCollectionProxy> g_font_collection;
IPC::Sender* g_sender_override = nullptr;

// Windows-only DirectWrite support. These warm up the DirectWrite paths
// before sandbox lock down to allow Skia access to the Font Manager service.
void CreateDirectWriteFactory(IDWriteFactory** factory) {
  typedef decltype(DWriteCreateFactory)* DWriteCreateFactoryProc;
  HMODULE dwrite_dll = LoadLibraryW(L"dwrite.dll");
  // TODO(scottmg): Temporary code to track crash in http://crbug.com/387867.
  if (!dwrite_dll) {
    DWORD load_library_get_last_error = GetLastError();
    base::debug::Alias(&dwrite_dll);
    base::debug::Alias(&load_library_get_last_error);
    CHECK(false);
  }

  // This shouldn't be necessary, but not having this causes breakage in
  // content_browsertests, and possibly other high-stress cases.
  PatchServiceManagerCalls();

  DWriteCreateFactoryProc dwrite_create_factory_proc =
      reinterpret_cast<DWriteCreateFactoryProc>(
          GetProcAddress(dwrite_dll, "DWriteCreateFactory"));
  // TODO(scottmg): Temporary code to track crash in http://crbug.com/387867.
  if (!dwrite_create_factory_proc) {
    DWORD get_proc_address_get_last_error = GetLastError();
    base::debug::Alias(&dwrite_create_factory_proc);
    base::debug::Alias(&get_proc_address_get_last_error);
    CHECK(false);
  }
  CHECK(SUCCEEDED(dwrite_create_factory_proc(
      DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory))));
}

HRESULT STDMETHODCALLTYPE StubFontCollection(IDWriteFactory* factory,
                                             IDWriteFontCollection** col,
                                             BOOL checkUpdates) {
  DCHECK(g_font_collection);
  g_font_collection.CopyTo(col);
  return S_OK;
}

// Copied from content/common/font_warmup_win.cc
void PatchDWriteFactory(IDWriteFactory* factory) {
  const unsigned int kGetSystemFontCollectionVTableIndex = 3;

  PROC* vtable = *reinterpret_cast<PROC**>(factory);
  PROC* function_ptr = &vtable[kGetSystemFontCollectionVTableIndex];
  void* stub_function = &StubFontCollection;
  base::win::ModifyCode(function_ptr, &stub_function, sizeof(PROC));
}

// Needed as a function for Bind()
IPC::Sender* GetSenderOverride() {
  return g_sender_override;
}

}  // namespace

void InitializeDWriteFontProxy(
    const base::Callback<IPC::Sender*(void)>& sender) {
  mswr::ComPtr<IDWriteFactory> factory;

  CreateDirectWriteFactory(&factory);

  if (!g_font_collection) {
    if (g_sender_override) {
      mswr::MakeAndInitialize<DWriteFontCollectionProxy>(
          &g_font_collection, factory.Get(), base::Bind(&GetSenderOverride));
    } else {
      mswr::MakeAndInitialize<DWriteFontCollectionProxy>(&g_font_collection,
                                                         factory.Get(), sender);
    }
  }

  PatchDWriteFactory(factory.Get());

  blink::WebFontRendering::setDirectWriteFactory(factory.Get());
  SkFontMgr* skia_font_manager = SkFontMgr_New_DirectWrite(factory.Get());
  SetDefaultSkiaFactory(skia_font_manager);
}

void UninitializeDWriteFontProxy() {
  if (g_font_collection)
    g_font_collection->Unregister();
}

void SetDWriteFontProxySenderForTesting(IPC::Sender* sender) {
  g_sender_override = sender;
}

}  // namespace content
