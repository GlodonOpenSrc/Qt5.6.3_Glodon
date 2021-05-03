// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "third_party/libpng/png.h"

struct BufState {
  const unsigned char *data;
  size_t bytes_left;
};

void user_read_data(png_structp png_ptr, png_bytep data, png_size_t length) {
  BufState* buf_state = static_cast<BufState*>(png_get_io_ptr(png_ptr));
  if (length > buf_state->bytes_left) {
    png_error(png_ptr, "read error");
  }
  memcpy(data, buf_state->data, length);
  buf_state->bytes_left -= length;
  buf_state->data += length;
}
static const int kPngHeaderSize = 8;

// Entry point for LibFuzzer.
// Roughly follows the libpng book example:
// http://www.libpng.org/pub/png/book/chapter13.html
extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  if (size < kPngHeaderSize) {
    return 0;
  }

  std::vector<unsigned char> v(data, data + size);
  if (!png_sig_cmp(v.data(), 0, kPngHeaderSize)) {
    // not a PNG.
    return 0;
  }

  png_structp png_ptr = png_create_read_struct
    (PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  assert(png_ptr);

  png_infop info_ptr = png_create_info_struct(png_ptr);
  assert(info_ptr);

  base::ScopedClosureRunner struct_deleter(base::Bind(
        &png_destroy_read_struct, &png_ptr, &info_ptr, nullptr));

  if (setjmp(png_ptr->jmpbuf)) {
    // error handling for libpng
    return 0;
  }

  // Setting up reading from buffer.
  std::unique_ptr<BufState> buf_state(new BufState());
  buf_state->data = data + kPngHeaderSize;
  buf_state->bytes_left = size - kPngHeaderSize;
  png_set_read_fn(png_ptr, buf_state.get(), user_read_data);
  png_set_sig_bytes(png_ptr, kPngHeaderSize);

  // Reading
  png_read_info(png_ptr, info_ptr);
  png_voidp row = png_malloc(png_ptr, png_get_rowbytes(png_ptr, info_ptr));
  base::ScopedClosureRunner png_deleter(base::Bind(
        &png_free, png_ptr, row));

  png_uint_32 width, height;
  int bit_depth, color_type, interlace_type, compression_type;
  int filter_type;

  if (!png_get_IHDR(png_ptr, info_ptr, &width, &height,
        &bit_depth, &color_type, &interlace_type,
        &compression_type, &filter_type)) {
    return 0;
  }

  int passes = png_set_interlace_handling(png_ptr);
  png_start_read_image(png_ptr);

  for (int pass = 0; pass < passes; ++pass) {
    for (png_uint_32 y = 0; y < height; ++y) {
      png_read_row(png_ptr, static_cast<png_bytep>(row), NULL);
    }
  }

  return 0;
}
