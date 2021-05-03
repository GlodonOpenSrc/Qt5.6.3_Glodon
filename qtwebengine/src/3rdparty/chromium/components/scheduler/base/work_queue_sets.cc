// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/scheduler/base/work_queue_sets.h"

#include "base/logging.h"
#include "components/scheduler/base/work_queue.h"

namespace scheduler {
namespace internal {

WorkQueueSets::WorkQueueSets(size_t num_sets)
    : enqueue_order_to_work_queue_maps_(num_sets) {}

WorkQueueSets::~WorkQueueSets() {}

void WorkQueueSets::RemoveQueue(WorkQueue* work_queue) {
  EnqueueOrder enqueue_order;
  bool has_enqueue_order = work_queue->GetFrontTaskEnqueueOrder(&enqueue_order);
  if (!has_enqueue_order)
    return;
  size_t set_index = work_queue->work_queue_set_index();
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size());
  DCHECK_EQ(
      work_queue,
      enqueue_order_to_work_queue_maps_[set_index].find(enqueue_order)->second);
  enqueue_order_to_work_queue_maps_[set_index].erase(enqueue_order);
}

void WorkQueueSets::AssignQueueToSet(WorkQueue* work_queue, size_t set_index) {
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size());
  EnqueueOrder enqueue_order;
  bool has_enqueue_order = work_queue->GetFrontTaskEnqueueOrder(&enqueue_order);
  size_t old_set = work_queue->work_queue_set_index();
  DCHECK_LT(old_set, enqueue_order_to_work_queue_maps_.size());
  work_queue->set_work_queue_set_index(set_index);
  if (!has_enqueue_order)
    return;
  enqueue_order_to_work_queue_maps_[old_set].erase(enqueue_order);
  enqueue_order_to_work_queue_maps_[set_index].insert(
      std::make_pair(enqueue_order, work_queue));
}

void WorkQueueSets::OnPushQueue(WorkQueue* work_queue) {
  EnqueueOrder enqueue_order;
  bool has_enqueue_order = work_queue->GetFrontTaskEnqueueOrder(&enqueue_order);
  DCHECK(has_enqueue_order);
  size_t set_index = work_queue->work_queue_set_index();
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size())
      << " set_index = " << set_index;
  enqueue_order_to_work_queue_maps_[set_index].insert(
      std::make_pair(enqueue_order, work_queue));
}

void WorkQueueSets::OnPopQueue(WorkQueue* work_queue) {
  size_t set_index = work_queue->work_queue_set_index();
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size());
  DCHECK(!enqueue_order_to_work_queue_maps_[set_index].empty())
      << " set_index = " << set_index;
  DCHECK_EQ(enqueue_order_to_work_queue_maps_[set_index].begin()->second,
            work_queue)
      << " set_index = " << set_index;
  // O(1) amortised.
  enqueue_order_to_work_queue_maps_[set_index].erase(
      enqueue_order_to_work_queue_maps_[set_index].begin());
  EnqueueOrder enqueue_order;
  bool has_enqueue_order = work_queue->GetFrontTaskEnqueueOrder(&enqueue_order);
  if (!has_enqueue_order)
    return;
  enqueue_order_to_work_queue_maps_[set_index].insert(
      std::make_pair(enqueue_order, work_queue));
}

bool WorkQueueSets::GetOldestQueueInSet(size_t set_index,
                                        WorkQueue** out_work_queue) const {
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size());
  if (enqueue_order_to_work_queue_maps_[set_index].empty())
    return false;
  *out_work_queue =
      enqueue_order_to_work_queue_maps_[set_index].begin()->second;
  return true;
}

bool WorkQueueSets::IsSetEmpty(size_t set_index) const {
  DCHECK_LT(set_index, enqueue_order_to_work_queue_maps_.size())
      << " set_index = " << set_index;
  return enqueue_order_to_work_queue_maps_[set_index].empty();
}

}  // namespace internal
}  // namespace scheduler
