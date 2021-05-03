// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/scheduler/renderer/throttling_helper.h"

#include "base/logging.h"
#include "components/scheduler/base/real_time_domain.h"
#include "components/scheduler/child/scheduler_tqm_delegate.h"
#include "components/scheduler/renderer/renderer_scheduler_impl.h"
#include "components/scheduler/renderer/throttled_time_domain.h"
#include "components/scheduler/renderer/web_frame_scheduler_impl.h"
#include "third_party/WebKit/public/platform/WebFrameScheduler.h"

namespace scheduler {

ThrottlingHelper::ThrottlingHelper(RendererSchedulerImpl* renderer_scheduler,
                                   const char* tracing_category)
    : task_runner_(renderer_scheduler->ControlTaskRunner()),
      renderer_scheduler_(renderer_scheduler),
      tick_clock_(renderer_scheduler->tick_clock()),
      tracing_category_(tracing_category),
      time_domain_(new ThrottledTimeDomain(this, tick_clock_)),
      weak_factory_(this) {
  suspend_timers_when_backgrounded_closure_.Reset(base::Bind(
      &ThrottlingHelper::PumpThrottledTasks, weak_factory_.GetWeakPtr()));
  forward_immediate_work_closure_ =
      base::Bind(&ThrottlingHelper::OnTimeDomainHasImmediateWork,
                 weak_factory_.GetWeakPtr());

  renderer_scheduler_->RegisterTimeDomain(time_domain_.get());
}

ThrottlingHelper::~ThrottlingHelper() {
  renderer_scheduler_->UnregisterTimeDomain(time_domain_.get());
}

void ThrottlingHelper::Throttle(TaskQueue* task_queue) {
  DCHECK_NE(task_queue, task_runner_.get());
  throttled_queues_.insert(task_queue);

  task_queue->SetTimeDomain(time_domain_.get());
  task_queue->SetPumpPolicy(TaskQueue::PumpPolicy::MANUAL);

  if (!task_queue->IsEmpty()) {
    if (task_queue->HasPendingImmediateWork()) {
      OnTimeDomainHasImmediateWork();
    } else {
      OnTimeDomainHasDelayedWork();
    }
  }
}

void ThrottlingHelper::Unthrottle(TaskQueue* task_queue) {
  throttled_queues_.erase(task_queue);

  task_queue->SetTimeDomain(renderer_scheduler_->real_time_domain());
  task_queue->SetPumpPolicy(TaskQueue::PumpPolicy::AUTO);
}

void ThrottlingHelper::OnTimeDomainHasImmediateWork() {
  // Forward to the main thread if called from another thread.
  if (!task_runner_->RunsTasksOnCurrentThread()) {
    task_runner_->PostTask(FROM_HERE, forward_immediate_work_closure_);
    return;
  }
  TRACE_EVENT0(tracing_category_,
               "ThrottlingHelper::OnTimeDomainHasImmediateWork");
  base::TimeTicks now = tick_clock_->NowTicks();
  MaybeSchedulePumpThrottledTasksLocked(FROM_HERE, now, now);
}

void ThrottlingHelper::OnTimeDomainHasDelayedWork() {
  TRACE_EVENT0(tracing_category_,
               "ThrottlingHelper::OnTimeDomainHasDelayedWork");
  base::TimeTicks next_scheduled_delayed_task;
  bool has_delayed_task =
      time_domain_->NextScheduledRunTime(&next_scheduled_delayed_task);
  DCHECK(has_delayed_task);
  base::TimeTicks now = tick_clock_->NowTicks();
  MaybeSchedulePumpThrottledTasksLocked(FROM_HERE, now,
                                        next_scheduled_delayed_task);
}

void ThrottlingHelper::PumpThrottledTasks() {
  TRACE_EVENT0(tracing_category_, "ThrottlingHelper::PumpThrottledTasks");
  pending_pump_throttled_tasks_runtime_ = base::TimeTicks();

  base::TimeTicks now = tick_clock_->NowTicks();
  time_domain_->AdvanceTo(now);
  for (TaskQueue* task_queue : throttled_queues_) {
    if (task_queue->IsEmpty())
      continue;

    task_queue->PumpQueue(false);
  }
  // Make sure NextScheduledRunTime gives us an up-to date result.
  time_domain_->ClearExpiredWakeups();

  base::TimeTicks next_scheduled_delayed_task;
  // Maybe schedule a call to ThrottlingHelper::PumpThrottledTasks if there is
  // a pending delayed task. NOTE posting a non-delayed task in the future will
  // result in ThrottlingHelper::OnTimeDomainHasImmediateWork being called.
  if (time_domain_->NextScheduledRunTime(&next_scheduled_delayed_task)) {
    MaybeSchedulePumpThrottledTasksLocked(FROM_HERE, now,
                                          next_scheduled_delayed_task);
  }
}

/* static */
base::TimeTicks ThrottlingHelper::ThrottledRunTime(
    base::TimeTicks unthrottled_runtime) {
  const base::TimeDelta one_second = base::TimeDelta::FromSeconds(1);
  return unthrottled_runtime + one_second -
      ((unthrottled_runtime - base::TimeTicks()) % one_second);
}

void ThrottlingHelper::MaybeSchedulePumpThrottledTasksLocked(
    const tracked_objects::Location& from_here,
    base::TimeTicks now,
    base::TimeTicks unthrottled_runtime) {
  base::TimeTicks throttled_runtime = ThrottledRunTime(unthrottled_runtime);
  // If there is a pending call to PumpThrottledTasks and it's sooner than
  // |unthrottled_runtime| then return.
  if (!pending_pump_throttled_tasks_runtime_.is_null() &&
      throttled_runtime >= pending_pump_throttled_tasks_runtime_) {
    return;
  }

  pending_pump_throttled_tasks_runtime_ = throttled_runtime;

  suspend_timers_when_backgrounded_closure_.Cancel();
  task_runner_->PostDelayedTask(
      from_here, suspend_timers_when_backgrounded_closure_.callback(),
      pending_pump_throttled_tasks_runtime_ - now);
}

}  // namespace scheduler
