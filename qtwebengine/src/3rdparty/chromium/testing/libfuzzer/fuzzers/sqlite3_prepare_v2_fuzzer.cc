// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include "third_party/sqlite/sqlite3.h"

static int Progress(void *not_used_ptr) {
  return 1;
}

// Entry point for LibFuzzer.
extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  if (size < 2)
    return 0;

  sqlite3* db;
  if (SQLITE_OK != sqlite3_open(":memory:", &db))
    return 0;

  // Use first byte as random selector for other parameters.
  int selector = data[0];

  // To cover both cases when progress_handler is used and isn't used.
  if (selector & 1)
    sqlite3_progress_handler(db, 4, &Progress, NULL);
  else
    sqlite3_progress_handler(db, 0, NULL, NULL);

  // Remove least significant bit to make further usage of selector independent.
  selector <<= 1;

  sqlite3_stmt* statement = NULL;
  int result = sqlite3_prepare_v2(db, (const char*)(data + 1), size - 1,
                                  &statement, NULL);
  if (result == SQLITE_OK) {
    // Use selector value to randomize number of iterations.
    for (int i = 0; i < selector; i++) {
      if (sqlite3_step(statement) != SQLITE_ROW)
        break;
    }

    sqlite3_finalize(statement);
  }

  sqlite3_close(db);
  return 0;
}
