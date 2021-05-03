/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "Benchmark.h"
#include "SkCanvas.h"
#include "SkImageSource.h"
#include "SkMergeImageFilter.h"
#include "SkSurface.h"

#define FILTER_WIDTH_SMALL  SkIntToScalar(32)
#define FILTER_HEIGHT_SMALL SkIntToScalar(32)
#define FILTER_WIDTH_LARGE  SkIntToScalar(256)
#define FILTER_HEIGHT_LARGE SkIntToScalar(256)

class MergeBench : public Benchmark {
public:
    MergeBench(bool small) : fIsSmall(small), fInitialized(false) { }

protected:
    const char* onGetName() override {
        return fIsSmall ? "merge_small" : "merge_large";
    }

    void onDelayedSetup() override {
        if (!fInitialized) {
            make_bitmap();
            make_checkerboard();
            fInitialized = true;
        }
    }

    void onDraw(int loops, SkCanvas* canvas) override {
        SkRect r = fIsSmall ? SkRect::MakeWH(FILTER_WIDTH_SMALL, FILTER_HEIGHT_SMALL) :
                              SkRect::MakeWH(FILTER_WIDTH_LARGE, FILTER_HEIGHT_LARGE);
        SkPaint paint;
        paint.setImageFilter(mergeBitmaps())->unref();
        for (int i = 0; i < loops; i++) {
            canvas->drawRect(r, paint);
        }
    }

private:
    SkImageFilter* mergeBitmaps() {
        SkAutoTUnref<SkImageFilter> first(SkImageSource::Create(fCheckerboard));
        SkAutoTUnref<SkImageFilter> second(SkImageSource::Create(fImage));
        return SkMergeImageFilter::Create(first, second);
    }

    void make_bitmap() {
        SkAutoTUnref<SkSurface> surface(SkSurface::NewRasterN32Premul(80, 80));
        surface->getCanvas()->clear(0x00000000);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(0xFF884422);
        paint.setTextSize(SkIntToScalar(96));
        const char* str = "g";
        surface->getCanvas()->drawText(str, strlen(str), 15, 55, paint);
        fImage.reset(surface->newImageSnapshot());
    }

    void make_checkerboard() {
        SkAutoTUnref<SkSurface> surface(SkSurface::NewRasterN32Premul(80, 80));
        SkCanvas* canvas = surface->getCanvas();
        canvas->clear(0x00000000);
        SkPaint darkPaint;
        darkPaint.setColor(0xFF804020);
        SkPaint lightPaint;
        lightPaint.setColor(0xFF244484);
        for (int y = 0; y < 80; y += 16) {
            for (int x = 0; x < 80; x += 16) {
                canvas->save();
                canvas->translate(SkIntToScalar(x), SkIntToScalar(y));
                canvas->drawRect(SkRect::MakeXYWH(0, 0, 8, 8), darkPaint);
                canvas->drawRect(SkRect::MakeXYWH(8, 0, 8, 8), lightPaint);
                canvas->drawRect(SkRect::MakeXYWH(0, 8, 8, 8), lightPaint);
                canvas->drawRect(SkRect::MakeXYWH(8, 8, 8, 8), darkPaint);
                canvas->restore();
            }
        }

        fCheckerboard.reset(surface->newImageSnapshot());
    }

    bool fIsSmall;
    bool fInitialized;
    SkAutoTUnref<SkImage> fImage, fCheckerboard;

    typedef Benchmark INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

DEF_BENCH( return new MergeBench(true); )
DEF_BENCH( return new MergeBench(false); )
