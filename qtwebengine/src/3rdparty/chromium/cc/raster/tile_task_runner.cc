// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/raster/tile_task_runner.h"

namespace cc {

TileTask::TileTask() : did_schedule_(false), did_complete_(false) {
}

TileTask::~TileTask() {
  DCHECK(!did_schedule_);
  DCHECK(!did_run_ || did_complete_);
}

void TileTask::WillSchedule() {
  DCHECK(!did_schedule_);
}

void TileTask::DidSchedule() {
  did_schedule_ = true;
  did_complete_ = false;
}

bool TileTask::HasBeenScheduled() const {
  return did_schedule_;
}

void TileTask::WillComplete() {
  DCHECK(!did_complete_);
}

void TileTask::DidComplete() {
  DCHECK(did_schedule_);
  DCHECK(!did_complete_);
  did_schedule_ = false;
  did_complete_ = true;
}

bool TileTask::HasCompleted() const {
  return did_complete_;
}

ImageDecodeTask::ImageDecodeTask() {
}

ImageDecodeTask::~ImageDecodeTask() {
}

RasterTask::RasterTask(ImageDecodeTask::Vector* dependencies) {
  dependencies_.swap(*dependencies);
}

RasterTask::~RasterTask() {
}

}  // namespace cc
