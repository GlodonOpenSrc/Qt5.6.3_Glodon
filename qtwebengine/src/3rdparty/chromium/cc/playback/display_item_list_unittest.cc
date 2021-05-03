// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/playback/display_item_list.h"

#include <stddef.h>

#include <vector>

#include "cc/output/filter_operation.h"
#include "cc/output/filter_operations.h"
#include "cc/playback/clip_display_item.h"
#include "cc/playback/clip_path_display_item.h"
#include "cc/playback/compositing_display_item.h"
#include "cc/playback/display_item_list_settings.h"
#include "cc/playback/drawing_display_item.h"
#include "cc/playback/filter_display_item.h"
#include "cc/playback/float_clip_display_item.h"
#include "cc/playback/transform_display_item.h"
#include "cc/proto/display_item.pb.h"
#include "cc/test/skia_common.h"
#include "skia/ext/refptr.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/core/SkXfermode.h"
#include "third_party/skia/include/effects/SkColorMatrixFilter.h"
#include "third_party/skia/include/effects/SkImageSource.h"
#include "third_party/skia/include/utils/SkPictureUtils.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/skia_util.h"

namespace cc {

namespace {

const gfx::Rect kVisualRect(0, 0, 42, 42);

void AppendFirstSerializationTestPicture(scoped_refptr<DisplayItemList> list,
                                         const gfx::Size& layer_size) {
  gfx::PointF offset(2.f, 3.f);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;

  SkPaint red_paint;
  red_paint.setColor(SK_ColorRED);

  canvas = skia::SharePtr(recorder.beginRecording(SkRect::MakeXYWH(
      offset.x(), offset.y(), layer_size.width(), layer_size.height())));
  canvas->translate(offset.x(), offset.y());
  canvas->drawRectCoords(0.f, 0.f, 4.f, 4.f, red_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));
}

void AppendSecondSerializationTestPicture(scoped_refptr<DisplayItemList> list,
                                          const gfx::Size& layer_size) {
  gfx::PointF offset(2.f, 2.f);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;

  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);

  canvas = skia::SharePtr(recorder.beginRecording(SkRect::MakeXYWH(
      offset.x(), offset.y(), layer_size.width(), layer_size.height())));
  canvas->translate(offset.x(), offset.y());
  canvas->drawRectCoords(3.f, 3.f, 7.f, 7.f, blue_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));
}

void ValidateDisplayItemListSerialization(const gfx::Size& layer_size,
                                          scoped_refptr<DisplayItemList> list) {
  list->Finalize();

  // Serialize and deserialize the DisplayItemList.
  proto::DisplayItemList proto;
  list->ToProtobuf(&proto);
  scoped_refptr<DisplayItemList> new_list =
      DisplayItemList::CreateFromProto(proto);

  EXPECT_TRUE(
      AreDisplayListDrawingResultsSame(gfx::Rect(layer_size), list, new_list));
}

}  // namespace

TEST(DisplayItemListTest, SerializeDisplayItemListSettings) {
  DisplayItemListSettings settings;
  settings.use_cached_picture = false;

  {
    proto::DisplayItemListSettings proto;
    settings.ToProtobuf(&proto);
    DisplayItemListSettings deserialized(proto);
    EXPECT_EQ(settings.use_cached_picture, deserialized.use_cached_picture);
  }

  settings.use_cached_picture = true;
  {
    proto::DisplayItemListSettings proto;
    settings.ToProtobuf(&proto);
    DisplayItemListSettings deserialized(proto);
    EXPECT_EQ(settings.use_cached_picture, deserialized.use_cached_picture);
  }
}

TEST(DisplayItemListTest, SerializeSingleDrawingItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SerializeClipItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  // Build the ClipDisplayItem.
  gfx::Rect clip_rect(6, 6, 1, 1);
  std::vector<SkRRect> rrects;
  rrects.push_back(SkRRect::MakeOval(SkRect::MakeXYWH(5.f, 5.f, 4.f, 4.f)));
  list->CreateAndAppendItem<ClipDisplayItem>(kVisualRect, clip_rect, rrects);

  // Build the second DrawingDisplayItem.
  AppendSecondSerializationTestPicture(list, layer_size);

  // Build the EndClipDisplayItem.
  list->CreateAndAppendItem<EndClipDisplayItem>(kVisualRect);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SerializeClipPathItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  // Build the ClipPathDisplayItem.
  SkPath path;
  path.addCircle(5.f, 5.f, 2.f, SkPath::Direction::kCW_Direction);
  list->CreateAndAppendItem<ClipPathDisplayItem>(
      kVisualRect, path, SkRegion::Op::kReplace_Op, false);

  // Build the second DrawingDisplayItem.
  AppendSecondSerializationTestPicture(list, layer_size);

  // Build the EndClipPathDisplayItem.
  list->CreateAndAppendItem<EndClipPathDisplayItem>(kVisualRect);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SerializeCompositingItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  // Build the CompositingDisplayItem.
  skia::RefPtr<SkColorFilter> filter = skia::AdoptRef(
      SkColorMatrixFilter::CreateLightingFilter(SK_ColorRED, SK_ColorGREEN));
  list->CreateAndAppendItem<CompositingDisplayItem>(
      kVisualRect, 150, SkXfermode::Mode::kDst_Mode, nullptr, filter, false);

  // Build the second DrawingDisplayItem.
  AppendSecondSerializationTestPicture(list, layer_size);

  // Build the EndCompositingDisplayItem.
  list->CreateAndAppendItem<EndCompositingDisplayItem>(kVisualRect);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SerializeFloatClipItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  // Build the FloatClipDisplayItem.
  gfx::RectF clip_rect(6.f, 6.f, 1.f, 1.f);
  list->CreateAndAppendItem<FloatClipDisplayItem>(kVisualRect, clip_rect);

  // Build the second DrawingDisplayItem.
  AppendSecondSerializationTestPicture(list, layer_size);

  // Build the EndFloatClipDisplayItem.
  list->CreateAndAppendItem<EndFloatClipDisplayItem>(kVisualRect);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SerializeTransformItem) {
  gfx::Size layer_size(10, 10);

  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(layer_size), settings);

  // Build the DrawingDisplayItem.
  AppendFirstSerializationTestPicture(list, layer_size);

  // Build the TransformDisplayItem.
  gfx::Transform transform;
  transform.Scale(1.25f, 1.25f);
  transform.Translate(-1.f, -1.f);
  list->CreateAndAppendItem<TransformDisplayItem>(kVisualRect, transform);

  // Build the second DrawingDisplayItem.
  AppendSecondSerializationTestPicture(list, layer_size);

  // Build the EndTransformDisplayItem.
  list->CreateAndAppendItem<EndTransformDisplayItem>(kVisualRect);

  ValidateDisplayItemListSerialization(layer_size, list);
}

TEST(DisplayItemListTest, SingleDrawingItem) {
  gfx::Rect layer_rect(100, 100);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;
  skia::RefPtr<SkPicture> picture;
  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);
  SkPaint red_paint;
  red_paint.setColor(SK_ColorRED);
  unsigned char pixels[4 * 100 * 100] = {0};
  DisplayItemListSettings settings;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);

  gfx::PointF offset(8.f, 9.f);
  gfx::RectF recording_rect(offset, gfx::SizeF(layer_rect.size()));
  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(recording_rect)));
  canvas->translate(offset.x(), offset.y());
  canvas->drawRectCoords(0.f, 0.f, 60.f, 60.f, red_paint);
  canvas->drawRectCoords(50.f, 50.f, 75.f, 75.f, blue_paint);
  picture = skia::AdoptRef(recorder.endRecordingAsPicture());
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect,
                                                std::move(picture));
  list->Finalize();
  DrawDisplayList(pixels, layer_rect, list);

  SkBitmap expected_bitmap;
  unsigned char expected_pixels[4 * 100 * 100] = {0};
  SkImageInfo info =
      SkImageInfo::MakeN32Premul(layer_rect.width(), layer_rect.height());
  expected_bitmap.installPixels(info, expected_pixels, info.minRowBytes());
  SkCanvas expected_canvas(expected_bitmap);
  expected_canvas.clipRect(gfx::RectToSkRect(layer_rect));
  expected_canvas.drawRectCoords(0.f + offset.x(), 0.f + offset.y(),
                                 60.f + offset.x(), 60.f + offset.y(),
                                 red_paint);
  expected_canvas.drawRectCoords(50.f + offset.x(), 50.f + offset.y(),
                                 75.f + offset.x(), 75.f + offset.y(),
                                 blue_paint);

  EXPECT_EQ(0, memcmp(pixels, expected_pixels, 4 * 100 * 100));
}

TEST(DisplayItemListTest, ClipItem) {
  gfx::Rect layer_rect(100, 100);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;
  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);
  SkPaint red_paint;
  red_paint.setColor(SK_ColorRED);
  unsigned char pixels[4 * 100 * 100] = {0};
  DisplayItemListSettings settings;
  settings.use_cached_picture = true;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);

  gfx::PointF first_offset(8.f, 9.f);
  gfx::RectF first_recording_rect(first_offset, gfx::SizeF(layer_rect.size()));
  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(first_recording_rect)));
  canvas->translate(first_offset.x(), first_offset.y());
  canvas->drawRectCoords(0.f, 0.f, 60.f, 60.f, red_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));

  gfx::Rect clip_rect(60, 60, 10, 10);
  list->CreateAndAppendItem<ClipDisplayItem>(kVisualRect, clip_rect,
                                             std::vector<SkRRect>());

  gfx::PointF second_offset(2.f, 3.f);
  gfx::RectF second_recording_rect(second_offset,
                                   gfx::SizeF(layer_rect.size()));
  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(second_recording_rect)));
  canvas->translate(second_offset.x(), second_offset.y());
  canvas->drawRectCoords(50.f, 50.f, 75.f, 75.f, blue_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));

  list->CreateAndAppendItem<EndClipDisplayItem>(kVisualRect);
  list->Finalize();

  DrawDisplayList(pixels, layer_rect, list);

  SkBitmap expected_bitmap;
  unsigned char expected_pixels[4 * 100 * 100] = {0};
  SkImageInfo info =
      SkImageInfo::MakeN32Premul(layer_rect.width(), layer_rect.height());
  expected_bitmap.installPixels(info, expected_pixels, info.minRowBytes());
  SkCanvas expected_canvas(expected_bitmap);
  expected_canvas.clipRect(gfx::RectToSkRect(layer_rect));
  expected_canvas.drawRectCoords(0.f + first_offset.x(), 0.f + first_offset.y(),
                                 60.f + first_offset.x(),
                                 60.f + first_offset.y(), red_paint);
  expected_canvas.clipRect(gfx::RectToSkRect(clip_rect));
  expected_canvas.drawRectCoords(
      50.f + second_offset.x(), 50.f + second_offset.y(),
      75.f + second_offset.x(), 75.f + second_offset.y(), blue_paint);

  EXPECT_EQ(0, memcmp(pixels, expected_pixels, 4 * 100 * 100));
}

TEST(DisplayItemListTest, TransformItem) {
  gfx::Rect layer_rect(100, 100);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;
  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);
  SkPaint red_paint;
  red_paint.setColor(SK_ColorRED);
  unsigned char pixels[4 * 100 * 100] = {0};
  DisplayItemListSettings settings;
  settings.use_cached_picture = true;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);

  gfx::PointF first_offset(8.f, 9.f);
  gfx::RectF first_recording_rect(first_offset, gfx::SizeF(layer_rect.size()));
  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(first_recording_rect)));
  canvas->translate(first_offset.x(), first_offset.y());
  canvas->drawRectCoords(0.f, 0.f, 60.f, 60.f, red_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));

  gfx::Transform transform;
  transform.Rotate(45.0);
  list->CreateAndAppendItem<TransformDisplayItem>(kVisualRect, transform);

  gfx::PointF second_offset(2.f, 3.f);
  gfx::RectF second_recording_rect(second_offset,
                                   gfx::SizeF(layer_rect.size()));
  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(second_recording_rect)));
  canvas->translate(second_offset.x(), second_offset.y());
  canvas->drawRectCoords(50.f, 50.f, 75.f, 75.f, blue_paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));

  list->CreateAndAppendItem<EndTransformDisplayItem>(kVisualRect);
  list->Finalize();

  DrawDisplayList(pixels, layer_rect, list);

  SkBitmap expected_bitmap;
  unsigned char expected_pixels[4 * 100 * 100] = {0};
  SkImageInfo info =
      SkImageInfo::MakeN32Premul(layer_rect.width(), layer_rect.height());
  expected_bitmap.installPixels(info, expected_pixels, info.minRowBytes());
  SkCanvas expected_canvas(expected_bitmap);
  expected_canvas.clipRect(gfx::RectToSkRect(layer_rect));
  expected_canvas.drawRectCoords(0.f + first_offset.x(), 0.f + first_offset.y(),
                                 60.f + first_offset.x(),
                                 60.f + first_offset.y(), red_paint);
  expected_canvas.setMatrix(transform.matrix());
  expected_canvas.drawRectCoords(
      50.f + second_offset.x(), 50.f + second_offset.y(),
      75.f + second_offset.x(), 75.f + second_offset.y(), blue_paint);

  EXPECT_EQ(0, memcmp(pixels, expected_pixels, 4 * 100 * 100));
}

TEST(DisplayItemListTest, FilterItem) {
  gfx::Rect layer_rect(100, 100);
  FilterOperations filters;
  unsigned char pixels[4 * 100 * 100] = {0};
  DisplayItemListSettings settings;
  settings.use_cached_picture = true;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);

  skia::RefPtr<SkSurface> source_surface =
      skia::AdoptRef(SkSurface::NewRasterN32Premul(50, 50));
  SkCanvas* source_canvas = source_surface->getCanvas();
  source_canvas->clear(SkColorSetRGB(128, 128, 128));
  skia::RefPtr<SkImage> source_image =
      skia::AdoptRef(source_surface->newImageSnapshot());

  // For most SkImageFilters, the |dst| bounds computed by computeFastBounds are
  // dependent on the provided |src| bounds. This means, for example, that
  // translating |src| results in a corresponding translation of |dst|. But this
  // is not the case for all SkImageFilters; for some of them (e.g.
  // SkImageSource), the computation of |dst| in computeFastBounds doesn't
  // involve |src| at all. Incorrectly assuming such a relationship (e.g. by
  // translating |dst| after it is computed by computeFastBounds, rather than
  // translating |src| before it provided to computedFastBounds) can cause
  // incorrect clipping of filter output. To test for this, we include an
  // SkImageSource filter in |filters|. Here, |src| is |filter_bounds|, defined
  // below.
  skia::RefPtr<SkImageFilter> image_filter =
      skia::AdoptRef(SkImageSource::Create(source_image.get()));
  filters.Append(FilterOperation::CreateReferenceFilter(image_filter));
  filters.Append(FilterOperation::CreateBrightnessFilter(0.5f));
  gfx::RectF filter_bounds(10.f, 10.f, 50.f, 50.f);
  list->CreateAndAppendItem<FilterDisplayItem>(kVisualRect, filters,
                                               filter_bounds);
  list->CreateAndAppendItem<EndFilterDisplayItem>(kVisualRect);
  list->Finalize();

  DrawDisplayList(pixels, layer_rect, list);

  SkBitmap expected_bitmap;
  unsigned char expected_pixels[4 * 100 * 100] = {0};
  SkPaint paint;
  paint.setColor(SkColorSetRGB(64, 64, 64));
  SkImageInfo info =
      SkImageInfo::MakeN32Premul(layer_rect.width(), layer_rect.height());
  expected_bitmap.installPixels(info, expected_pixels, info.minRowBytes());
  SkCanvas expected_canvas(expected_bitmap);
  expected_canvas.drawRect(RectFToSkRect(filter_bounds), paint);

  EXPECT_EQ(0, memcmp(pixels, expected_pixels, 4 * 100 * 100));
}

TEST(DisplayItemListTest, CompactingItems) {
  gfx::Rect layer_rect(100, 100);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;
  skia::RefPtr<SkPicture> picture;
  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);
  SkPaint red_paint;
  red_paint.setColor(SK_ColorRED);
  unsigned char pixels[4 * 100 * 100] = {0};

  gfx::PointF offset(8.f, 9.f);
  gfx::RectF recording_rect(offset, gfx::SizeF(layer_rect.size()));

  DisplayItemListSettings no_caching_settings;
  no_caching_settings.use_cached_picture = false;
  scoped_refptr<DisplayItemList> list_without_caching =
      DisplayItemList::Create(layer_rect, no_caching_settings);

  canvas = skia::SharePtr(
      recorder.beginRecording(gfx::RectFToSkRect(recording_rect)));
  canvas->translate(offset.x(), offset.y());
  canvas->drawRectCoords(0.f, 0.f, 60.f, 60.f, red_paint);
  canvas->drawRectCoords(50.f, 50.f, 75.f, 75.f, blue_paint);
  picture = skia::AdoptRef(recorder.endRecordingAsPicture());
  list_without_caching->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect,
                                                                picture);
  list_without_caching->Finalize();
  DrawDisplayList(pixels, layer_rect, list_without_caching);

  unsigned char expected_pixels[4 * 100 * 100] = {0};
  DisplayItemListSettings caching_settings;
  caching_settings.use_cached_picture = true;
  scoped_refptr<DisplayItemList> list_with_caching =
      DisplayItemList::Create(layer_rect, caching_settings);
  list_with_caching->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect,
                                                             picture);
  list_with_caching->Finalize();
  DrawDisplayList(expected_pixels, layer_rect, list_with_caching);

  EXPECT_EQ(0, memcmp(pixels, expected_pixels, 4 * 100 * 100));
}

TEST(DisplayItemListTest, IsSuitableForGpuRasterizationWithCachedPicture) {
  gfx::Rect layer_rect(1000, 1000);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;

  DisplayItemListSettings settings;
  settings.use_cached_picture = true;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);
  canvas =
      skia::SharePtr(recorder.beginRecording(gfx::RectToSkRect(layer_rect)));

  SkPath path;
  path.moveTo(0, 0);
  path.lineTo(0, 100);
  path.lineTo(50, 50);
  path.lineTo(100, 100);
  path.lineTo(100, 0);
  path.close();

  SkPaint paint;
  paint.setAntiAlias(true);
  canvas->drawPath(path, paint);

  skia::RefPtr<SkPicture> suitable_picture =
      skia::AdoptRef(recorder.endRecordingAsPicture());
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect, suitable_picture);
  list->Finalize();

  // A single DrawingDisplayItem with a large AA concave path shouldn't trigger
  // a veto.
  EXPECT_TRUE(list->IsSuitableForGpuRasterization());

  // Now check the RasterIntoCanvas path.
  list = DisplayItemList::Create(layer_rect, settings);
  DrawingDisplayItem suitable_item(suitable_picture);
  list->RasterIntoCanvas(suitable_item);
  list->Finalize();
  EXPECT_TRUE(list->IsSuitableForGpuRasterization());

  list = DisplayItemList::Create(layer_rect, settings);
  canvas =
      skia::SharePtr(recorder.beginRecording(gfx::RectToSkRect(layer_rect)));
  for (int i = 0; i < 10; ++i)
    canvas->drawPath(path, paint);
  skia::RefPtr<SkPicture> unsuitable_picture =
      skia::AdoptRef(recorder.endRecordingAsPicture());
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect,
                                                unsuitable_picture);
  list->Finalize();

  // A single DrawingDisplayItem with several large AA concave paths should
  // trigger a veto.
  EXPECT_FALSE(list->IsSuitableForGpuRasterization());

  // Now check the RasterIntoCanvas path.
  list = DisplayItemList::Create(layer_rect, settings);
  DrawingDisplayItem unsuitable_item(unsuitable_picture);
  list->RasterIntoCanvas(unsuitable_item);
  list->Finalize();
  EXPECT_FALSE(list->IsSuitableForGpuRasterization());
}

TEST(DisplayItemListTest, IsSuitableForGpuRasterizationWithoutCachedPicture) {
  gfx::Rect layer_rect(1000, 1000);
  SkPictureRecorder recorder;
  skia::RefPtr<SkCanvas> canvas;
  skia::RefPtr<SkPicture> picture;

  DisplayItemListSettings settings;
  settings.use_cached_picture = false;
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, settings);
  canvas =
      skia::SharePtr(recorder.beginRecording(gfx::RectToSkRect(layer_rect)));

  SkPath path;
  path.moveTo(0, 0);
  path.lineTo(0, 100);
  path.lineTo(50, 50);
  path.lineTo(100, 100);
  path.lineTo(100, 0);
  path.close();

  SkPaint paint;
  paint.setAntiAlias(true);
  canvas->drawPath(path, paint);

  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));
  list->Finalize();

  // A single DrawingDisplayItem with a large AA concave path shouldn't trigger
  // a veto.
  EXPECT_TRUE(list->IsSuitableForGpuRasterization());

  list = DisplayItemList::Create(layer_rect, settings);
  canvas =
      skia::SharePtr(recorder.beginRecording(gfx::RectToSkRect(layer_rect)));
  for (int i = 0; i < 10; ++i)
    canvas->drawPath(path, paint);
  list->CreateAndAppendItem<DrawingDisplayItem>(
      kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));
  list->Finalize();

  // A single DrawingDisplayItem with several large AA concave paths should
  // trigger a veto.
  EXPECT_FALSE(list->IsSuitableForGpuRasterization());

  list = DisplayItemList::Create(layer_rect, settings);
  for (int i = 0; i < 10; ++i) {
    canvas =
        skia::SharePtr(recorder.beginRecording(gfx::RectToSkRect(layer_rect)));
    canvas->drawPath(path, paint);
    list->CreateAndAppendItem<DrawingDisplayItem>(
        kVisualRect, skia::AdoptRef(recorder.endRecordingAsPicture()));
  }
  list->Finalize();

  // Without a cached picture, having several DrawingDisplayItems that each
  // contain a single large AA concave will not trigger a veto, since each item
  // is individually suitable for GPU rasterization.
  EXPECT_TRUE(list->IsSuitableForGpuRasterization());
}

TEST(DisplayItemListTest, ApproximateMemoryUsage) {
  const int kNumCommandsInTestSkPicture = 1000;
  scoped_refptr<DisplayItemList> list;
  size_t memory_usage;

  // Make an SkPicture whose size is known.
  gfx::Rect layer_rect(100, 100);
  SkPictureRecorder recorder;
  SkPaint blue_paint;
  blue_paint.setColor(SK_ColorBLUE);
  SkCanvas* canvas = recorder.beginRecording(gfx::RectToSkRect(layer_rect));
  for (int i = 0; i < kNumCommandsInTestSkPicture; i++)
    canvas->drawPaint(blue_paint);
  skia::RefPtr<SkPicture> picture =
      skia::AdoptRef(recorder.endRecordingAsPicture());
  size_t picture_size = SkPictureUtils::ApproximateBytesUsed(picture.get());
  ASSERT_GE(picture_size, kNumCommandsInTestSkPicture * sizeof(blue_paint));

  // Using a cached picture, we should get about the right size.
  DisplayItemListSettings caching_settings;
  caching_settings.use_cached_picture = true;
  list = DisplayItemList::Create(layer_rect, caching_settings);
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect, picture);
  list->Finalize();
  memory_usage = list->ApproximateMemoryUsage();
  EXPECT_GE(memory_usage, picture_size);
  EXPECT_LE(memory_usage, 2 * picture_size);

  // Using no cached picture, we should still get the right size.
  DisplayItemListSettings no_caching_settings;
  no_caching_settings.use_cached_picture = false;
  list = DisplayItemList::Create(layer_rect, no_caching_settings);
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect, picture);
  list->Finalize();
  memory_usage = list->ApproximateMemoryUsage();
  EXPECT_GE(memory_usage, picture_size);
  EXPECT_LE(memory_usage, 2 * picture_size);

  // To avoid double counting, we expect zero size to be computed if both the
  // picture and items are retained (currently this only happens due to certain
  // categories being traced).
  list = new DisplayItemList(layer_rect, caching_settings, true);
  list->CreateAndAppendItem<DrawingDisplayItem>(kVisualRect, picture);
  list->Finalize();
  memory_usage = list->ApproximateMemoryUsage();
  EXPECT_EQ(static_cast<size_t>(0), memory_usage);
}

TEST(DisplayItemListTest, AsValueWithRectAndNoItems) {
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(1, 2, 8, 9), DisplayItemListSettings());
  list->Finalize();

  std::string value = list->AsValue(true)->ToString();
  EXPECT_NE(value.find("\"items\":[]"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[1,2,8,9]"), std::string::npos);
  EXPECT_NE(value.find("\"skp64\":"), std::string::npos);

  value = list->AsValue(false)->ToString();
  EXPECT_EQ(value.find("\"items\":"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[1,2,8,9]"), std::string::npos);
  EXPECT_NE(value.find("\"skp64\":"), std::string::npos);
}

TEST(DisplayItemListTest, AsValueWithRectAndItems) {
  gfx::Rect layer_rect = gfx::Rect(1, 2, 8, 9);
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(layer_rect, DisplayItemListSettings());
  gfx::Transform transform;
  transform.Translate(6.f, 7.f);
  list->CreateAndAppendItem<TransformDisplayItem>(kVisualRect, transform);
  AppendFirstSerializationTestPicture(list, layer_rect.size());
  list->CreateAndAppendItem<EndTransformDisplayItem>(kVisualRect);
  list->Finalize();

  std::string value = list->AsValue(true)->ToString();
  EXPECT_NE(value.find("{\"items\":[\"TransformDisplayItem"),
            std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[1,2,8,9]"), std::string::npos);
  EXPECT_NE(value.find("\"skp64\":"), std::string::npos);

  value = list->AsValue(false)->ToString();
  EXPECT_EQ(value.find("{\"items\":[\"TransformDisplayItem"),
            std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[1,2,8,9]"), std::string::npos);
  EXPECT_NE(value.find("\"skp64\":"), std::string::npos);
}

TEST(DisplayItemListTest, AsValueWithEmptyRectAndNoItems) {
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(), DisplayItemListSettings());
  list->Finalize();

  std::string value = list->AsValue(true)->ToString();
  EXPECT_NE(value.find("\"items\":[]"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[0,0,0,0]"), std::string::npos);
  EXPECT_EQ(value.find("\"skp64\":"), std::string::npos);

  value = list->AsValue(false)->ToString();
  EXPECT_EQ(value.find("\"items\":"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[0,0,0,0]"), std::string::npos);
  EXPECT_EQ(value.find("\"skp64\":"), std::string::npos);
}

TEST(DisplayItemListTest, AsValueWithEmptyRectAndItems) {
  scoped_refptr<DisplayItemList> list =
      DisplayItemList::Create(gfx::Rect(), DisplayItemListSettings());
  gfx::Transform transform;
  transform.Translate(6.f, 7.f);
  list->CreateAndAppendItem<TransformDisplayItem>(kVisualRect, transform);
  AppendFirstSerializationTestPicture(list, gfx::Size());
  list->CreateAndAppendItem<EndTransformDisplayItem>(kVisualRect);
  list->Finalize();

  std::string value = list->AsValue(true)->ToString();
  EXPECT_NE(value.find("\"items\":[\"TransformDisplayItem"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[0,0,0,0]"), std::string::npos);
  // There should be one skp64 entry present associated with the test picture
  // item, though the overall list has no skp64 as the layer rect is empty.
  EXPECT_NE(value.find("\"skp64\":"), std::string::npos);

  value = list->AsValue(false)->ToString();
  EXPECT_EQ(value.find("\"items\":"), std::string::npos);
  EXPECT_NE(value.find("\"layer_rect\":[0,0,0,0]"), std::string::npos);
  // There should be no skp64 entry present as the items aren't included and the
  // layer rect is empty.
  EXPECT_EQ(value.find("\"skp64\":"), std::string::npos);
}

}  // namespace cc
