// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <set>
#include <string>

#include "base/json/json_reader.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/trace_event/heap_profiler_allocation_context.h"
#include "base/trace_event/heap_profiler_heap_dump_writer.h"
#include "base/trace_event/heap_profiler_stack_frame_deduplicator.h"
#include "base/trace_event/heap_profiler_type_name_deduplicator.h"
#include "base/trace_event/trace_event_argument.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Define all strings once, because the deduplicator requires pointer equality,
// and string interning is unreliable.
const char kBrowserMain[] = "BrowserMain";
const char kRendererMain[] = "RendererMain";
const char kCreateWidget[] = "CreateWidget";
const char kInitialize[] = "Initialize";

const char kInt[] = "int";
const char kBool[] = "bool";
const char kString[] = "string";

}  // namespace

namespace base {
namespace trace_event {
namespace internal {

scoped_ptr<const Value> WriteAndReadBack(const std::set<Entry>& entries) {
  scoped_refptr<TracedValue> traced_value = Serialize(entries);
  std::string json;
  traced_value->AppendAsTraceFormat(&json);
  return JSONReader::Read(json);
}

scoped_ptr<const DictionaryValue> WriteAndReadBackEntry(Entry entry) {
  std::set<Entry> input_entries;
  input_entries.insert(entry);

  scoped_ptr<const Value> json_dict = WriteAndReadBack(input_entries);

  // Note: Ideally these should use |ASSERT_TRUE| instead of |EXPECT_TRUE|, but
  // |ASSERT_TRUE| can only be used in void functions.
  const DictionaryValue* dictionary;
  EXPECT_TRUE(json_dict->GetAsDictionary(&dictionary));

  const ListValue* json_entries;
  EXPECT_TRUE(dictionary->GetList("entries", &json_entries));

  const DictionaryValue* json_entry;
  EXPECT_TRUE(json_entries->GetDictionary(0, &json_entry));

  return json_entry->CreateDeepCopy();
}

// Given a desired stack frame ID and type ID, looks up the entry in the set and
// asserts that it is present and has the expected size.
void AssertSizeEq(const std::set<Entry>& entries,
                  int stack_frame_id,
                  int type_id,
                  size_t expected_size) {
  // The comparison operator for |Entry| does not take size into account, so by
  // setting only stack frame ID and type ID, the real entry can be found.
  Entry entry;
  entry.stack_frame_id = stack_frame_id;
  entry.type_id = type_id;
  auto it = entries.find(entry);

  ASSERT_NE(entries.end(), it) << "No entry found for sf = " << stack_frame_id
                               << ", type = " << type_id << ".";
  ASSERT_EQ(expected_size, it->size) << "Wrong size for sf = " << stack_frame_id
                                     << ", type = " << type_id << ".";
}

TEST(HeapDumpWriterTest, BacktraceIndex) {
  Entry entry;
  entry.stack_frame_id = -1;  // -1 means empty backtrace.
  entry.type_id = 0;
  entry.size = 1;

  scoped_ptr<const DictionaryValue> json_entry = WriteAndReadBackEntry(entry);

  // For an empty backtrace, the "bt" key cannot reference a stack frame.
  // Instead it should be set to the empty string.
  std::string backtrace_index;
  ASSERT_TRUE(json_entry->GetString("bt", &backtrace_index));
  ASSERT_EQ("", backtrace_index);

  // Also verify that a non-negative backtrace index is dumped properly.
  entry.stack_frame_id = 2;
  json_entry = WriteAndReadBackEntry(entry);
  ASSERT_TRUE(json_entry->GetString("bt", &backtrace_index));
  ASSERT_EQ("2", backtrace_index);
}

TEST(HeapDumpWriterTest, TypeId) {
  Entry entry;
  entry.type_id = -1;  // -1 means sum over all types.
  entry.stack_frame_id = 0;
  entry.size = 1;

  scoped_ptr<const DictionaryValue> json_entry = WriteAndReadBackEntry(entry);

  // Entries for the cumulative size of all types should not have the "type"
  // key set.
  ASSERT_FALSE(json_entry->HasKey("type"));

  // Also verify that a non-negative type ID is dumped properly.
  entry.type_id = 2;
  json_entry = WriteAndReadBackEntry(entry);
  std::string type_id;
  ASSERT_TRUE(json_entry->GetString("type", &type_id));
  ASSERT_EQ("2", type_id);
}

TEST(HeapDumpWriterTest, SizeIsHexadecimalString) {
  // Take a number between 2^63 and 2^64 (or between 2^31 and 2^32 if |size_t|
  // is not 64 bits).
  const size_t large_value =
      sizeof(size_t) == 8 ? 0xffffffffffffffc5 : 0xffffff9d;
  const char* large_value_str =
      sizeof(size_t) == 8 ? "ffffffffffffffc5" : "ffffff9d";
  Entry entry;
  entry.type_id = 0;
  entry.stack_frame_id = 0;
  entry.size = large_value;

  scoped_ptr<const DictionaryValue> json_entry = WriteAndReadBackEntry(entry);

  std::string size;
  ASSERT_TRUE(json_entry->GetString("size", &size));
  ASSERT_EQ(large_value_str, size);
}

TEST(HeapDumpWriterTest, BacktraceTypeNameTable) {
  hash_map<AllocationContext, size_t> bytes_by_context;

  AllocationContext ctx = AllocationContext::Empty();
  ctx.backtrace.frames[0] = kBrowserMain;
  ctx.backtrace.frames[1] = kCreateWidget;
  ctx.type_name = kInt;

  // 10 bytes with context { type: int, bt: [BrowserMain, CreateWidget] }.
  bytes_by_context[ctx] = 10;

  ctx.type_name = kBool;

  // 18 bytes with context { type: bool, bt: [BrowserMain, CreateWidget] }.
  bytes_by_context[ctx] = 18;

  ctx.backtrace.frames[0] = kRendererMain;
  ctx.backtrace.frames[1] = kInitialize;

  // 30 bytes with context { type: bool, bt: [RendererMain, Initialize] }.
  bytes_by_context[ctx] = 30;

  ctx.type_name = kString;

  // 19 bytes with context { type: string, bt: [RendererMain, Initialize] }.
  bytes_by_context[ctx] = 19;

  // At this point the heap looks like this:
  //
  // |        | CrWidget <- BrMain | Init <- RenMain | Sum |
  // +--------+--------------------+-----------------+-----+
  // | int    |                 10 |               0 |  10 |
  // | bool   |                 18 |              30 |  48 |
  // | string |                  0 |              19 |  19 |
  // +--------+--------------------+-----------------+-----+
  // | Sum    |                 28 |              49 |  77 |

  auto sf_deduplicator = make_scoped_refptr(new StackFrameDeduplicator);
  auto tn_deduplicator = make_scoped_refptr(new TypeNameDeduplicator);
  HeapDumpWriter writer(sf_deduplicator.get(), tn_deduplicator.get());
  const std::set<Entry>& dump = writer.Summarize(bytes_by_context);

  // Get the indices of the backtraces and types by adding them again to the
  // deduplicator. Because they were added before, the same number is returned.
  StackFrame bt0[] = {kRendererMain, kInitialize};
  StackFrame bt1[] = {kBrowserMain, kCreateWidget};
  int bt_renderer_main = sf_deduplicator->Insert(bt0, bt0 + 1);
  int bt_browser_main = sf_deduplicator->Insert(bt1, bt1 + 1);
  int bt_renderer_main_initialize = sf_deduplicator->Insert(bt0, bt0 + 2);
  int bt_browser_main_create_widget = sf_deduplicator->Insert(bt1, bt1 + 2);
  int type_id_int = tn_deduplicator->Insert(kInt);
  int type_id_bool = tn_deduplicator->Insert(kBool);
  int type_id_string = tn_deduplicator->Insert(kString);

  // Full heap should have size 77.
  AssertSizeEq(dump, -1, -1, 77);

  // 49 bytes were allocated in RendererMain and children. Also check the type
  // breakdown.
  AssertSizeEq(dump, bt_renderer_main, -1, 49);
  AssertSizeEq(dump, bt_renderer_main, type_id_bool, 30);
  AssertSizeEq(dump, bt_renderer_main, type_id_string, 19);

  // 28 bytes were allocated in BrowserMain and children. Also check the type
  // breakdown.
  AssertSizeEq(dump, bt_browser_main, -1, 28);
  AssertSizeEq(dump, bt_browser_main, type_id_int, 10);
  AssertSizeEq(dump, bt_browser_main, type_id_bool, 18);

  // In this test all bytes are allocated in leaf nodes, so check again one
  // level deeper.
  AssertSizeEq(dump, bt_renderer_main_initialize, -1, 49);
  AssertSizeEq(dump, bt_renderer_main_initialize, type_id_bool, 30);
  AssertSizeEq(dump, bt_renderer_main_initialize, type_id_string, 19);
  AssertSizeEq(dump, bt_browser_main_create_widget, -1, 28);
  AssertSizeEq(dump, bt_browser_main_create_widget, type_id_int, 10);
  AssertSizeEq(dump, bt_browser_main_create_widget, type_id_bool, 18);

  // The type breakdown of the entrie heap should have been dumped as well.
  AssertSizeEq(dump, -1, type_id_int, 10);
  AssertSizeEq(dump, -1, type_id_bool, 48);
  AssertSizeEq(dump, -1, type_id_string, 19);
}

// TODO(ruuda): Verify that cumulative sizes are computed correctly.
// TODO(ruuda): Verify that insignificant values are not dumped.

}  // namespace internal
}  // namespace trace_event
}  // namespace base
