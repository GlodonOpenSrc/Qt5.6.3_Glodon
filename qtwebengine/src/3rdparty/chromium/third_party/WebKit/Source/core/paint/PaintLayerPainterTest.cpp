// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/layout/compositing/CompositedLayerMapping.h"
#include "core/paint/PaintControllerPaintTest.h"
#include "platform/graphics/GraphicsContext.h"

namespace blink {

class PaintLayerPainterTest
    : public PaintControllerPaintTest
    , public testing::WithParamInterface<FrameSettingOverrideFunction> {
    USING_FAST_MALLOC(PaintLayerPainterTest);
public:
    FrameSettingOverrideFunction settingOverrider() const override { return GetParam(); }
};

INSTANTIATE_TEST_CASE_P(All, PaintLayerPainterTest, ::testing::Values(
    nullptr,
    RootLayerScrollsFrameSettingOverride));

TEST_P(PaintLayerPainterTest, CachedSubsequence)
{
    setBodyInnerHTML(
        "<div id='container1' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content1' style='position: absolute; width: 100px; height: 100px; background-color: red'></div>"
        "</div>"
        "<div id='container2' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content2' style='position: absolute; width: 100px; height: 100px; background-color: green'></div>"
        "</div>");
    document().view()->updateAllLifecyclePhases();

    PaintLayer& htmlLayer = *toLayoutBoxModelObject(document().documentElement()->layoutObject())->layer();
    LayoutObject& container1 = *document().getElementById("container1")->layoutObject();
    PaintLayer& container1Layer = *toLayoutBoxModelObject(container1).layer();
    LayoutObject& content1 = *document().getElementById("content1")->layoutObject();
    LayoutObject& container2 = *document().getElementById("container2")->layoutObject();
    PaintLayer& container2Layer = *toLayoutBoxModelObject(container2).layer();
    LayoutObject& content2 = *document().getElementById("content2")->layoutObject();

    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 11,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    toHTMLElement(content1.node())->setAttribute(HTMLNames::styleAttr, "position: absolute; width: 100px; height: 100px; background-color: green");
    updateLifecyclePhasesBeforePaint();
    bool needsCommit = paintWithoutCommit();

    EXPECT_DISPLAY_LIST(rootPaintController().newDisplayItemList(), 8,
        TestDisplayItem(layoutView(), cachedBackgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, cachedBackgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::CachedSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    if (needsCommit)
        commit();

    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 11,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));
}

TEST_P(PaintLayerPainterTest, CachedSubsequenceOnInterestRectChange)
{
    setBodyInnerHTML(
        "<div id='container1' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content1' style='position: absolute; width: 100px; height: 100px; background-color: green'></div>"
        "</div>"
        "<div id='container2' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content2a' style='position: absolute; width: 100px; height: 100px; background-color: green'></div>"
        "  <div id='content2b' style='position: absolute; top: 200px; width: 100px; height: 100px; background-color: green'></div>"
        "</div>"
        "<div id='container3' style='position: absolute; z-index: 2; left: 300px; top: 0; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content3' style='position: absolute; width: 200px; height: 200px; background-color: green'></div>"
        "</div>");
    rootPaintController().invalidateAll();

    PaintLayer& htmlLayer = *toLayoutBoxModelObject(document().documentElement()->layoutObject())->layer();
    LayoutObject& container1 = *document().getElementById("container1")->layoutObject();
    PaintLayer& container1Layer = *toLayoutBoxModelObject(container1).layer();
    LayoutObject& content1 = *document().getElementById("content1")->layoutObject();
    LayoutObject& container2 = *document().getElementById("container2")->layoutObject();
    PaintLayer& container2Layer = *toLayoutBoxModelObject(container2).layer();
    LayoutObject& content2a = *document().getElementById("content2a")->layoutObject();
    LayoutObject& content2b = *document().getElementById("content2b")->layoutObject();
    LayoutObject& container3 = *document().getElementById("container3")->layoutObject();
    PaintLayer& container3Layer = *toLayoutBoxModelObject(container3).layer();
    LayoutObject& content3 = *document().getElementById("content3")->layoutObject();

    updateLifecyclePhasesBeforePaint();
    IntRect interestRect(0, 0, 400, 300);
    paint(&interestRect);

    // Container1 is fully in the interest rect;
    // Container2 is partly (including its stacking chidren) in the interest rect;
    // Content2b is out of the interest rect and output nothing;
    // Container3 is partly in the interest rect.
    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 15,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2a, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container3Layer, DisplayItem::Subsequence),
        TestDisplayItem(container3, backgroundType),
        TestDisplayItem(content3, backgroundType),
        TestDisplayItem(container3Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    updateLifecyclePhasesBeforePaint();
    IntRect newInterestRect(0, 100, 300, 1000);
    bool needsCommit = paintWithoutCommit(&newInterestRect);

    // Container1 becomes partly in the interest rect, but uses cached subsequence
    // because it was fully painted before;
    // Container2's intersection with the interest rect changes;
    // Content2b is out of the interest rect and outputs nothing;
    // Container3 becomes out of the interest rect and outputs empty subsequence pair..
    EXPECT_DISPLAY_LIST(rootPaintController().newDisplayItemList(), 11,
        TestDisplayItem(layoutView(), cachedBackgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::CachedSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, cachedBackgroundType),
        TestDisplayItem(content2a, cachedBackgroundType),
        TestDisplayItem(content2b, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container3Layer, DisplayItem::Subsequence),
        TestDisplayItem(container3Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    if (needsCommit)
        commit();

    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 14,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2a, backgroundType),
        TestDisplayItem(content2b, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container3Layer, DisplayItem::Subsequence),
        TestDisplayItem(container3Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));
}

TEST_P(PaintLayerPainterTest, CachedSubsequenceOnStyleChangeWithInterestRectClipping)
{
    setBodyInnerHTML(
        "<div id='container1' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content1' style='position: absolute; width: 100px; height: 100px; background-color: red'></div>"
        "</div>"
        "<div id='container2' style='position: relative; z-index: 1; width: 200px; height: 200px; background-color: blue'>"
        "  <div id='content2' style='position: absolute; width: 100px; height: 100px; background-color: green'></div>"
        "</div>");
    updateLifecyclePhasesBeforePaint();
    IntRect interestRect(0, 0, 50, 300); // PaintResult of all subsequences will be MayBeClippedByPaintDirtyRect.
    paint(&interestRect);

    PaintLayer& htmlLayer = *toLayoutBoxModelObject(document().documentElement()->layoutObject())->layer();
    LayoutObject& container1 = *document().getElementById("container1")->layoutObject();
    PaintLayer& container1Layer = *toLayoutBoxModelObject(container1).layer();
    LayoutObject& content1 = *document().getElementById("content1")->layoutObject();
    LayoutObject& container2 = *document().getElementById("container2")->layoutObject();
    PaintLayer& container2Layer = *toLayoutBoxModelObject(container2).layer();
    LayoutObject& content2 = *document().getElementById("content2")->layoutObject();

    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 11,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    toHTMLElement(content1.node())->setAttribute(HTMLNames::styleAttr, "position: absolute; width: 100px; height: 100px; background-color: green");
    updateLifecyclePhasesBeforePaint();
    bool needsCommit = paintWithoutCommit(&interestRect);

    EXPECT_DISPLAY_LIST(rootPaintController().newDisplayItemList(), 8,
        TestDisplayItem(layoutView(), cachedBackgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, cachedBackgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::CachedSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));

    if (needsCommit)
        commit();

    EXPECT_DISPLAY_LIST(rootPaintController().displayItemList(), 11,
        TestDisplayItem(layoutView(), backgroundType),
        TestDisplayItem(htmlLayer, DisplayItem::Subsequence),
        TestDisplayItem(container1Layer, DisplayItem::Subsequence),
        TestDisplayItem(container1, backgroundType),
        TestDisplayItem(content1, backgroundType),
        TestDisplayItem(container1Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(container2Layer, DisplayItem::Subsequence),
        TestDisplayItem(container2, backgroundType),
        TestDisplayItem(content2, backgroundType),
        TestDisplayItem(container2Layer, DisplayItem::EndSubsequence),
        TestDisplayItem(htmlLayer, DisplayItem::EndSubsequence));
}

} // namespace blink
