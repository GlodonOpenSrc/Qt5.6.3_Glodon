// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/shared_memory.h"

#include <aclapi.h>
#include <stddef.h>
#include <stdint.h>

#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "base/rand_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"

namespace {

typedef enum _SECTION_INFORMATION_CLASS {
  SectionBasicInformation,
} SECTION_INFORMATION_CLASS;

typedef struct _SECTION_BASIC_INFORMATION {
  PVOID BaseAddress;
  ULONG Attributes;
  LARGE_INTEGER Size;
} SECTION_BASIC_INFORMATION, *PSECTION_BASIC_INFORMATION;

typedef ULONG(__stdcall* NtQuerySectionType)(
    HANDLE SectionHandle,
    SECTION_INFORMATION_CLASS SectionInformationClass,
    PVOID SectionInformation,
    ULONG SectionInformationLength,
    PULONG ResultLength);

// Returns the length of the memory section starting at the supplied address.
size_t GetMemorySectionSize(void* address) {
  MEMORY_BASIC_INFORMATION memory_info;
  if (!::VirtualQuery(address, &memory_info, sizeof(memory_info)))
    return 0;
  return memory_info.RegionSize - (static_cast<char*>(address) -
         static_cast<char*>(memory_info.AllocationBase));
}

// Checks if the section object is safe to map. At the moment this just means
// it's not an image section.
bool IsSectionSafeToMap(HANDLE handle) {
  static NtQuerySectionType nt_query_section_func;
  if (!nt_query_section_func) {
    nt_query_section_func = reinterpret_cast<NtQuerySectionType>(
        ::GetProcAddress(::GetModuleHandle(L"ntdll.dll"), "NtQuerySection"));
    DCHECK(nt_query_section_func);
  }

  // The handle must have SECTION_QUERY access for this to succeed.
  SECTION_BASIC_INFORMATION basic_information = {};
  ULONG status =
      nt_query_section_func(handle, SectionBasicInformation, &basic_information,
                            sizeof(basic_information), nullptr);
  if (status)
    return false;
  return (basic_information.Attributes & SEC_IMAGE) != SEC_IMAGE;
}

}  // namespace.

namespace base {

SharedMemoryCreateOptions::SharedMemoryCreateOptions()
    : name_deprecated(nullptr),
      open_existing_deprecated(false),
      size(0),
      executable(false),
      share_read_only(false) {}

SharedMemory::SharedMemory()
    : external_section_(false),
      mapped_file_(NULL),
      mapped_size_(0),
      memory_(NULL),
      read_only_(false),
      requested_size_(0) {}

SharedMemory::SharedMemory(const std::wstring& name)
    : external_section_(false),
      name_(name),
      mapped_file_(NULL),
      mapped_size_(0),
      memory_(NULL),
      read_only_(false),
      requested_size_(0) {}

SharedMemory::SharedMemory(const SharedMemoryHandle& handle, bool read_only)
    : external_section_(true),
      mapped_file_(handle.GetHandle()),
      mapped_size_(0),
      memory_(NULL),
      read_only_(read_only),
      requested_size_(0) {
  DCHECK(!handle.IsValid() || handle.BelongsToCurrentProcess());
}

SharedMemory::SharedMemory(const SharedMemoryHandle& handle,
                           bool read_only,
                           ProcessHandle process)
    : external_section_(true),
      mapped_file_(NULL),
      mapped_size_(0),
      memory_(NULL),
      read_only_(read_only),
      requested_size_(0) {
  DWORD access = FILE_MAP_READ | SECTION_QUERY;
  ::DuplicateHandle(process, handle.GetHandle(), GetCurrentProcess(),
                    &mapped_file_,
                    read_only_ ? access : access | FILE_MAP_WRITE, FALSE, 0);
}

SharedMemory::~SharedMemory() {
  Unmap();
  Close();
}

// static
bool SharedMemory::IsHandleValid(const SharedMemoryHandle& handle) {
  return handle.IsValid();
}

// static
SharedMemoryHandle SharedMemory::NULLHandle() {
  return SharedMemoryHandle();
}

// static
void SharedMemory::CloseHandle(const SharedMemoryHandle& handle) {
  handle.Close();
}

// static
size_t SharedMemory::GetHandleLimit() {
  // Rounded down from value reported here:
  // http://blogs.technet.com/b/markrussinovich/archive/2009/09/29/3283844.aspx
  return static_cast<size_t>(1 << 23);
}

// static
SharedMemoryHandle SharedMemory::DuplicateHandle(
    const SharedMemoryHandle& handle) {
  DCHECK(handle.BelongsToCurrentProcess());
  HANDLE duped_handle;
  ProcessHandle process = GetCurrentProcess();
  BOOL success =
      ::DuplicateHandle(process, handle.GetHandle(), process, &duped_handle, 0,
                        FALSE, DUPLICATE_SAME_ACCESS);
  if (success)
    return SharedMemoryHandle(duped_handle, GetCurrentProcId());
  return SharedMemoryHandle();
}

bool SharedMemory::CreateAndMapAnonymous(size_t size) {
  return CreateAnonymous(size) && Map(size);
}

bool SharedMemory::Create(const SharedMemoryCreateOptions& options) {
  // TODO(bsy,sehr): crbug.com/210609 NaCl forces us to round up 64k here,
  // wasting 32k per mapping on average.
  static const size_t kSectionMask = 65536 - 1;
  DCHECK(!options.executable);
  DCHECK(!mapped_file_);
  if (options.size == 0)
    return false;

  // Check maximum accounting for overflow.
  if (options.size >
      static_cast<size_t>(std::numeric_limits<int>::max()) - kSectionMask)
    return false;

  size_t rounded_size = (options.size + kSectionMask) & ~kSectionMask;
  name_ = options.name_deprecated ?
      ASCIIToUTF16(*options.name_deprecated) : L"";
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
  SECURITY_DESCRIPTOR sd;
  ACL dacl;

  if (options.share_read_only && name_.empty()) {
    // Add an empty DACL to enforce anonymous read-only sections.
    sa.lpSecurityDescriptor = &sd;
    if (!InitializeAcl(&dacl, sizeof(dacl), ACL_REVISION))
      return false;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
      return false;
    if (!SetSecurityDescriptorDacl(&sd, TRUE, &dacl, FALSE))
      return false;

    // Windows ignores DACLs on certain unnamed objects (like shared sections).
    // So, we generate a random name when we need to enforce read-only.
    uint64_t rand_values[4];
    RandBytes(&rand_values, sizeof(rand_values));
    name_ = StringPrintf(L"CrSharedMem_%016llx%016llx%016llx%016llx",
                         rand_values[0], rand_values[1],
                         rand_values[2], rand_values[3]);
  }
  mapped_file_ = CreateFileMapping(INVALID_HANDLE_VALUE, &sa,
      PAGE_READWRITE, 0, static_cast<DWORD>(rounded_size),
      name_.empty() ? nullptr : name_.c_str());
  if (!mapped_file_)
    return false;

  requested_size_ = options.size;

  // Check if the shared memory pre-exists.
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // If the file already existed, set requested_size_ to 0 to show that
    // we don't know the size.
    requested_size_ = 0;
    external_section_ = true;
    if (!options.open_existing_deprecated) {
      Close();
      return false;
    }
  }

  return true;
}

bool SharedMemory::Delete(const std::string& name) {
  // intentionally empty -- there is nothing for us to do on Windows.
  return true;
}

bool SharedMemory::Open(const std::string& name, bool read_only) {
  DCHECK(!mapped_file_);
  DWORD access = FILE_MAP_READ | SECTION_QUERY;
  if (!read_only)
    access |= FILE_MAP_WRITE;
  name_ = ASCIIToUTF16(name);
  read_only_ = read_only;
  mapped_file_ =
      OpenFileMapping(access, false, name_.empty() ? nullptr : name_.c_str());
  if (!mapped_file_)
    return false;
  // If a name specified assume it's an external section.
  if (!name_.empty())
    external_section_ = true;
  // Note: size_ is not set in this case.
  return true;
}

bool SharedMemory::MapAt(off_t offset, size_t bytes) {
  if (mapped_file_ == NULL)
    return false;

  if (bytes > static_cast<size_t>(std::numeric_limits<int>::max()))
    return false;

  if (memory_)
    return false;

  if (external_section_ && !IsSectionSafeToMap(mapped_file_))
    return false;

  memory_ = MapViewOfFile(
      mapped_file_, read_only_ ? FILE_MAP_READ : FILE_MAP_READ | FILE_MAP_WRITE,
      static_cast<uint64_t>(offset) >> 32, static_cast<DWORD>(offset), bytes);
  if (memory_ != NULL) {
    DCHECK_EQ(0U, reinterpret_cast<uintptr_t>(memory_) &
        (SharedMemory::MAP_MINIMUM_ALIGNMENT - 1));
    mapped_size_ = GetMemorySectionSize(memory_);
    return true;
  }
  return false;
}

bool SharedMemory::Unmap() {
  if (memory_ == NULL)
    return false;

  UnmapViewOfFile(memory_);
  memory_ = NULL;
  return true;
}

bool SharedMemory::ShareToProcessCommon(ProcessHandle process,
                                        SharedMemoryHandle* new_handle,
                                        bool close_self,
                                        ShareMode share_mode) {
  *new_handle = SharedMemoryHandle();
  DWORD access = FILE_MAP_READ | SECTION_QUERY;
  DWORD options = 0;
  HANDLE mapped_file = mapped_file_;
  HANDLE result;
  if (share_mode == SHARE_CURRENT_MODE && !read_only_)
    access |= FILE_MAP_WRITE;
  if (close_self) {
    // DUPLICATE_CLOSE_SOURCE causes DuplicateHandle to close mapped_file.
    options = DUPLICATE_CLOSE_SOURCE;
    mapped_file_ = NULL;
    Unmap();
  }

  if (process == GetCurrentProcess() && close_self) {
    *new_handle = SharedMemoryHandle(mapped_file, base::GetCurrentProcId());
    return true;
  }

  if (!::DuplicateHandle(GetCurrentProcess(), mapped_file, process, &result,
                         access, FALSE, options)) {
    return false;
  }
  *new_handle = SharedMemoryHandle(result, base::GetProcId(process));
  return true;
}


void SharedMemory::Close() {
  if (mapped_file_ != NULL) {
    ::CloseHandle(mapped_file_);
    mapped_file_ = NULL;
  }
}

SharedMemoryHandle SharedMemory::handle() const {
  return SharedMemoryHandle(mapped_file_, base::GetCurrentProcId());
}

}  // namespace base
