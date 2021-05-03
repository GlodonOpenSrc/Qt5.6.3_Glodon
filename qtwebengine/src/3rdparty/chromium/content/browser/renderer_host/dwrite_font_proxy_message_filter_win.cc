// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/renderer_host/dwrite_font_proxy_message_filter_win.h"

#include <dwrite.h>
#include <shlobj.h>
#include <stddef.h>
#include <stdint.h>

#include <set>
#include <utility>

#include "base/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string16.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/common/dwrite_font_proxy_messages.h"
#include "ipc/ipc_message_macros.h"
#include "ui/gfx/win/direct_write.h"

namespace mswr = Microsoft::WRL;

namespace content {

namespace {

// This enum is used to define the buckets for an enumerated UMA histogram.
// Hence,
//   (a) existing enumerated constants should never be deleted or reordered, and
//   (b) new constants should only be appended at the end of the enumeration.
enum DirectWriteFontLoaderType {
  FILE_SYSTEM_FONT_DIR = 0,
  FILE_OUTSIDE_SANDBOX = 1,
  OTHER_LOADER = 2,

  FONT_LOADER_TYPE_MAX_VALUE
};

void LogLoaderType(DirectWriteFontLoaderType loader_type) {
  UMA_HISTOGRAM_ENUMERATION("DirectWrite.Fonts.Proxy.LoaderType", loader_type,
                            FONT_LOADER_TYPE_MAX_VALUE);
}

const wchar_t* kFontsToIgnore[] = {
    // "Gill Sans Ultra Bold" turns into an Ultra Bold weight "Gill Sans" in
    // DirectWrite, but most users don't have any other weights. The regular
    // weight font is named "Gill Sans MT", but that ends up in a different
    // family with that name. On Mac, there's a "Gill Sans" with various
    // weights, so CSS authors use { 'font-family': 'Gill Sans',
    // 'Gill Sans MT', ... } and because of the DirectWrite family futzing,
    // they end up with an Ultra Bold font, when they just wanted "Gill Sans".
    // Mozilla implemented a more complicated hack where they effectively
    // rename the Ultra Bold font to "Gill Sans MT Ultra Bold", but because the
    // Ultra Bold font is so ugly anyway, we simply ignore it. See
    // http://www.microsoft.com/typography/fonts/font.aspx?FMID=978 for a
    // picture of the font, and the file name. We also ignore "Gill Sans Ultra
    // Bold Condensed".
    L"gilsanub.ttf", L"gillubcd.ttf",
};

base::string16 GetWindowsFontsPath() {
  std::vector<base::char16> font_path_chars;
  // SHGetSpecialFolderPath requires at least MAX_PATH characters.
  font_path_chars.resize(MAX_PATH);
  BOOL result = SHGetSpecialFolderPath(nullptr /* hwndOwner - reserved */,
                                       font_path_chars.data(), CSIDL_FONTS,
                                       FALSE /* fCreate */);
  DCHECK(result);
  return base::i18n::FoldCase(font_path_chars.data());
}

}  // namespace

DWriteFontProxyMessageFilter::DWriteFontProxyMessageFilter()
    : BrowserMessageFilter(DWriteFontProxyMsgStart),
      windows_fonts_path_(GetWindowsFontsPath()) {}

DWriteFontProxyMessageFilter::~DWriteFontProxyMessageFilter() = default;

bool DWriteFontProxyMessageFilter::OnMessageReceived(
    const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(DWriteFontProxyMessageFilter, message)
    IPC_MESSAGE_HANDLER(DWriteFontProxyMsg_FindFamily, OnFindFamily)
    IPC_MESSAGE_HANDLER(DWriteFontProxyMsg_GetFamilyCount, OnGetFamilyCount)
    IPC_MESSAGE_HANDLER(DWriteFontProxyMsg_GetFamilyNames, OnGetFamilyNames)
    IPC_MESSAGE_HANDLER(DWriteFontProxyMsg_GetFontFiles, OnGetFontFiles)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void DWriteFontProxyMessageFilter::OverrideThreadForMessage(
    const IPC::Message& message,
    content::BrowserThread::ID* thread) {
  if (IPC_MESSAGE_CLASS(message) == DWriteFontProxyMsgStart)
    *thread = BrowserThread::FILE;
}

void DWriteFontProxyMessageFilter::OnFindFamily(
    const base::string16& family_name,
    UINT32* family_index) {
  InitializeDirectWrite();
  TRACE_EVENT0("dwrite", "FontProxyHost::OnFindFamily");
  DCHECK(collection_);
  *family_index = UINT32_MAX;
  if (collection_) {
    BOOL exists = FALSE;
    UINT32 index = UINT32_MAX;
    HRESULT hr =
        collection_->FindFamilyName(family_name.data(), &index, &exists);
    if (SUCCEEDED(hr) && exists)
      *family_index = index;
  }
}

void DWriteFontProxyMessageFilter::OnGetFamilyCount(UINT32* count) {
  InitializeDirectWrite();
  TRACE_EVENT0("dwrite", "FontProxyHost::OnGetFamilyCount");
  DCHECK(collection_);
  if (!collection_)
    *count = 0;
  else
    *count = collection_->GetFontFamilyCount();
}

void DWriteFontProxyMessageFilter::OnGetFamilyNames(
    UINT32 family_index,
    std::vector<DWriteStringPair>* family_names) {
  InitializeDirectWrite();
  TRACE_EVENT0("dwrite", "FontProxyHost::OnGetFamilyNames");
  DCHECK(collection_);
  if (!collection_)
    return;

  TRACE_EVENT0("dwrite", "FontProxyHost::DoGetFamilyNames");

  mswr::ComPtr<IDWriteFontFamily> family;
  HRESULT hr = collection_->GetFontFamily(family_index, &family);
  if (!SUCCEEDED(hr))
    return;

  mswr::ComPtr<IDWriteLocalizedStrings> localized_names;
  hr = family->GetFamilyNames(&localized_names);
  if (!SUCCEEDED(hr))
    return;

  size_t string_count = localized_names->GetCount();

  std::vector<base::char16> locale;
  std::vector<base::char16> name;
  for (size_t index = 0; index < string_count; ++index) {
    UINT32 length = 0;
    hr = localized_names->GetLocaleNameLength(index, &length);
    if (!SUCCEEDED(hr))
      return;
    ++length;  // Reserve space for the null terminator.
    locale.resize(length);
    hr = localized_names->GetLocaleName(index, locale.data(), length);
    if (!SUCCEEDED(hr))
      return;
    DCHECK_EQ(L'\0', locale[length - 1]);

    length = 0;
    hr = localized_names->GetStringLength(index, &length);
    if (!SUCCEEDED(hr))
      return;
    ++length;  // Reserve space for the null terminator.
    name.resize(length);
    hr = localized_names->GetString(index, name.data(), length);
    if (!SUCCEEDED(hr))
      return;
    DCHECK_EQ(L'\0', name[length - 1]);

    // Would be great to use emplace_back instead.
    family_names->push_back(std::pair<base::string16, base::string16>(
        base::string16(locale.data()), base::string16(name.data())));
  }
}

void DWriteFontProxyMessageFilter::OnGetFontFiles(
    uint32_t family_index,
    std::vector<base::string16>* file_paths) {
  InitializeDirectWrite();
  TRACE_EVENT0("dwrite", "FontProxyHost::OnGetFontFiles");
  DCHECK(collection_);
  if (!collection_)
    return;

  mswr::ComPtr<IDWriteFontFamily> family;
  HRESULT hr = collection_->GetFontFamily(family_index, &family);
  if (!SUCCEEDED(hr))
    return;

  UINT32 font_count = family->GetFontCount();

  std::set<base::string16> path_set;
  // Iterate through all the fonts in the family, and all the files for those
  // fonts. If anything goes wrong, bail on the entire family to avoid having
  // a partially-loaded font family.
  for (UINT32 font_index = 0; font_index < font_count; ++font_index) {
    mswr::ComPtr<IDWriteFont> font;
    hr = family->GetFont(font_index, &font);
    if (!SUCCEEDED(hr))
      return;

    AddFilesForFont(&path_set, font.Get());
  }

  file_paths->assign(path_set.begin(), path_set.end());
}

void DWriteFontProxyMessageFilter::InitializeDirectWrite() {
  DCHECK_CURRENTLY_ON(BrowserThread::FILE);
  if (direct_write_initialized_)
    return;
  direct_write_initialized_ = true;

  mswr::ComPtr<IDWriteFactory> factory;
  gfx::win::CreateDWriteFactory(&factory);
  if (factory == nullptr) {
    // We won't be able to load fonts, but we should still return messages so
    // renderers don't hang if they for some reason send us a font message.
    return;
  }

  HRESULT hr = factory->GetSystemFontCollection(&collection_);
  DCHECK(SUCCEEDED(hr));
}

bool DWriteFontProxyMessageFilter::AddFilesForFont(
    std::set<base::string16>* path_set,
    IDWriteFont* font) {
  mswr::ComPtr<IDWriteFontFace> font_face;
  HRESULT hr;
  hr = font->CreateFontFace(&font_face);
  if (!SUCCEEDED(hr))
    return false;

  UINT32 file_count;
  hr = font_face->GetFiles(&file_count, nullptr);
  if (!SUCCEEDED(hr))
    return false;

  std::vector<mswr::ComPtr<IDWriteFontFile>> font_files;
  font_files.resize(file_count);
  hr = font_face->GetFiles(
      &file_count, reinterpret_cast<IDWriteFontFile**>(font_files.data()));
  if (!SUCCEEDED(hr))
    return false;

  for (unsigned int file_index = 0; file_index < file_count; ++file_index) {
    mswr::ComPtr<IDWriteFontFileLoader> loader;
    hr = font_files[file_index]->GetLoader(&loader);
    if (!SUCCEEDED(hr))
      return false;

    mswr::ComPtr<IDWriteLocalFontFileLoader> local_loader;
    hr = loader.CopyTo(local_loader.GetAddressOf());  // QueryInterface.

    if (hr == E_NOINTERFACE) {
      // We could get here if the system font collection contains fonts that
      // are backed by something other than files in the system fonts folder.
      // I don't think that is actually possible, so for now we'll just
      // ignore it (result will be that we'll be unable to match any styles
      // for this font, forcing blink/skia to fall back to whatever font is
      // next). If we get telemetry indicating that this case actually
      // happens, we can implement this by exposing the loader via ipc. That
      // will likely by loading the font data into shared memory, although we
      // could proxy the stream reads directly instead.
      LogLoaderType(OTHER_LOADER);
      DCHECK(false);
      return false;
    } else if (!SUCCEEDED(hr)) {
      return false;
    }

    if (!AddLocalFile(path_set, local_loader.Get(),
                      font_files[file_index].Get())) {
      return false;
    }
  }
  return true;
}

bool DWriteFontProxyMessageFilter::AddLocalFile(
    std::set<base::string16>* path_set,
    IDWriteLocalFontFileLoader* local_loader,
    IDWriteFontFile* font_file) {
  HRESULT hr;
  const void* key;
  UINT32 key_size;
  hr = font_file->GetReferenceKey(&key, &key_size);
  if (!SUCCEEDED(hr))
    return false;

  UINT32 path_length = 0;
  hr = local_loader->GetFilePathLengthFromKey(key, key_size, &path_length);
  if (!SUCCEEDED(hr))
    return false;
  ++path_length;  // Reserve space for the null terminator.
  std::vector<base::char16> file_path_chars;
  file_path_chars.resize(path_length);
  hr = local_loader->GetFilePathFromKey(key, key_size, file_path_chars.data(),
                                        path_length);
  if (!SUCCEEDED(hr))
    return false;

  base::string16 file_path = base::i18n::FoldCase(file_path_chars.data());
  if (!base::StartsWith(file_path, windows_fonts_path_,
                        base::CompareCase::SENSITIVE)) {
    // Skip loading fonts from outside the system fonts directory, since
    // these families will not be accessible to the renderer process. If
    // this turns out to be a common case, we can either grant the renderer
    // access to these files (not sure if this is actually possible), or
    // load the file data ourselves and hand it to the renderer.
    LogLoaderType(FILE_OUTSIDE_SANDBOX);
    NOTREACHED();  // Not yet implemented.
    return false;
  }

  // Refer to comments in kFontsToIgnore for this block.
  for (const auto& file_to_ignore : kFontsToIgnore) {
    // Ok to do ascii comparison since the strings we are looking for are
    // all ascii.
    if (base::EndsWith(file_path, file_to_ignore,
                       base::CompareCase::INSENSITIVE_ASCII)) {
      // Unlike most other cases in this function, we do not abort loading
      // the entire family, since we want to specifically ignore particular
      // font styles and load the rest of the family if it exists. The
      // renderer can deal with a family with zero files if that ends up
      // being the case.
      return true;
    }
  }

  LogLoaderType(FILE_SYSTEM_FONT_DIR);
  path_set->insert(file_path);
  return true;
}

}  // namespace content
