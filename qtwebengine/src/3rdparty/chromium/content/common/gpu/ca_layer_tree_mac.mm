// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/common/gpu/ca_layer_tree_mac.h"

#include "base/command_line.h"
#include "base/mac/sdk_forward_declarations.h"
#include "base/trace_event/trace_event.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/cocoa/animation_utils.h"
#include "ui/base/ui_base_switches.h"
#include "ui/gfx/geometry/dip_util.h"

namespace content {

CALayerTree::CALayerTree() {}
CALayerTree::~CALayerTree() {}

bool CALayerTree::ScheduleCALayer(
    bool is_clipped,
    const gfx::Rect& clip_rect,
    unsigned sorting_context_id,
    const gfx::Transform& transform,
    base::ScopedCFTypeRef<IOSurfaceRef> io_surface,
    const gfx::RectF& contents_rect,
    const gfx::Rect& rect,
    unsigned background_color,
    unsigned edge_aa_mask,
    float opacity) {
  // Excessive logging to debug white screens (crbug.com/583805).
  // TODO(ccameron): change this back to a DLOG.
  if (has_committed_) {
    LOG(ERROR) << "ScheduleCALayer called after CommitScheduledCALayers.";
    return false;
  }
  return root_layer_.AddContentLayer(is_clipped, clip_rect, sorting_context_id,
                                     transform, io_surface, contents_rect, rect,
                                     background_color, edge_aa_mask, opacity);
}

void CALayerTree::CommitScheduledCALayers(CALayer* superlayer,
                                          scoped_ptr<CALayerTree> old_tree,
                                          float scale_factor) {
  TRACE_EVENT0("gpu", "CALayerTree::CommitScheduledCALayers");
  RootLayer* old_root_layer = nullptr;
  if (old_tree) {
    DCHECK(old_tree->has_committed_);
    if (old_tree->scale_factor_ == scale_factor)
      old_root_layer = &old_tree->root_layer_;
  }

  root_layer_.CommitToCA(superlayer, old_root_layer, scale_factor);
  // If there are any extra CALayers in |old_tree| that were not stolen by this
  // tree, they will be removed from the CALayer tree in this deallocation.
  old_tree.reset();
  has_committed_ = true;
  scale_factor_ = scale_factor;
}

CALayerTree::RootLayer::RootLayer() {}

// Note that for all destructors, the the CALayer will have been reset to nil if
// another layer has taken it.
CALayerTree::RootLayer::~RootLayer() {
  [ca_layer removeFromSuperlayer];
}

CALayerTree::ClipAndSortingLayer::ClipAndSortingLayer(
    bool is_clipped,
    gfx::Rect clip_rect,
    unsigned sorting_context_id,
    bool is_singleton_sorting_context)
    : is_clipped(is_clipped),
      clip_rect(clip_rect),
      sorting_context_id(sorting_context_id),
      is_singleton_sorting_context(is_singleton_sorting_context) {}

CALayerTree::ClipAndSortingLayer::ClipAndSortingLayer(
    ClipAndSortingLayer&& layer)
    : transform_layers(std::move(layer.transform_layers)),
      is_clipped(layer.is_clipped),
      clip_rect(layer.clip_rect),
      sorting_context_id(layer.sorting_context_id),
      is_singleton_sorting_context(
          layer.is_singleton_sorting_context),
      ca_layer(layer.ca_layer) {
  // Ensure that the ca_layer be reset, so that when the destructor is called,
  // the layer hierarchy is unaffected.
  // TODO(ccameron): Add a move constructor for scoped_nsobject to do this
  // automatically.
  layer.ca_layer.reset();
}

CALayerTree::ClipAndSortingLayer::~ClipAndSortingLayer() {
  [ca_layer removeFromSuperlayer];
}

CALayerTree::TransformLayer::TransformLayer(const gfx::Transform& transform)
    : transform(transform) {}

CALayerTree::TransformLayer::TransformLayer(TransformLayer&& layer)
    : transform(layer.transform),
      content_layers(std::move(layer.content_layers)),
      ca_layer(layer.ca_layer) {
  layer.ca_layer.reset();
}

CALayerTree::TransformLayer::~TransformLayer() {
  [ca_layer removeFromSuperlayer];
}

CALayerTree::ContentLayer::ContentLayer(
    base::ScopedCFTypeRef<IOSurfaceRef> io_surface,
    const gfx::RectF& contents_rect,
    const gfx::Rect& rect,
    unsigned background_color,
    unsigned edge_aa_mask,
    float opacity)
    : io_surface(io_surface),
      contents_rect(contents_rect),
      rect(rect),
      background_color(background_color),
      ca_edge_aa_mask(0),
      opacity(opacity) {
  // Because the root layer has setGeometryFlipped:YES, there is some ambiguity
  // about what exactly top and bottom mean. This ambiguity is resolved in
  // different ways for solid color CALayers and for CALayers that have content
  // (surprise!). For CALayers with IOSurface content, the top edge in the AA
  // mask refers to what appears as the bottom edge on-screen. For CALayers
  // without content (solid color layers), the top edge in the AA mask is the
  // top edge on-screen.
  // http://crbug.com/567946
  if (edge_aa_mask & GL_CA_LAYER_EDGE_LEFT_CHROMIUM)
    ca_edge_aa_mask |= kCALayerLeftEdge;
  if (edge_aa_mask & GL_CA_LAYER_EDGE_RIGHT_CHROMIUM)
    ca_edge_aa_mask |= kCALayerRightEdge;
  if (io_surface) {
    if (edge_aa_mask & GL_CA_LAYER_EDGE_TOP_CHROMIUM)
      ca_edge_aa_mask |= kCALayerBottomEdge;
    if (edge_aa_mask & GL_CA_LAYER_EDGE_BOTTOM_CHROMIUM)
      ca_edge_aa_mask |= kCALayerTopEdge;
  } else {
    if (edge_aa_mask & GL_CA_LAYER_EDGE_TOP_CHROMIUM)
      ca_edge_aa_mask |= kCALayerTopEdge;
    if (edge_aa_mask & GL_CA_LAYER_EDGE_BOTTOM_CHROMIUM)
      ca_edge_aa_mask |= kCALayerBottomEdge;
  }

  // Ensure that the IOSurface be in use as soon as it is added to a
  // ContentLayer, so that, by the time that the call to SwapBuffers completes,
  // all IOSurfaces that can be used as CALayer contents in the future will be
  // marked as InUse.
  if (io_surface)
    IOSurfaceIncrementUseCount(io_surface);
}

CALayerTree::ContentLayer::ContentLayer(ContentLayer&& layer)
    : io_surface(layer.io_surface),
      contents_rect(layer.contents_rect),
      rect(layer.rect),
      background_color(layer.background_color),
      ca_edge_aa_mask(layer.ca_edge_aa_mask),
      opacity(layer.opacity),
      ca_layer(layer.ca_layer) {
  DCHECK(!layer.ca_layer);
  layer.ca_layer.reset();
  // See remarks in the non-move constructor.
  if (io_surface)
    IOSurfaceIncrementUseCount(io_surface);
}

CALayerTree::ContentLayer::~ContentLayer() {
  [ca_layer removeFromSuperlayer];
  // By the time the destructor is called, the IOSurface will have been passed
  // to the WindowServer, and will remain InUse by the WindowServer as long as
  // is needed to avoid recycling bugs.
  if (io_surface)
    IOSurfaceDecrementUseCount(io_surface);
}

bool CALayerTree::RootLayer::AddContentLayer(
    bool is_clipped,
    const gfx::Rect& clip_rect,
    unsigned sorting_context_id,
    const gfx::Transform& transform,
    base::ScopedCFTypeRef<IOSurfaceRef> io_surface,
    const gfx::RectF& contents_rect,
    const gfx::Rect& rect,
    unsigned background_color,
    unsigned edge_aa_mask,
    float opacity) {
  bool needs_new_clip_and_sorting_layer = true;

  // In sorting_context_id 0, all quads are listed in back-to-front order.
  // This is accomplished by having the CALayers be siblings of each other.
  // If a quad has a 3D transform, it is necessary to put it in its own sorting
  // context, so that it will not intersect with quads before and after it.
  bool is_singleton_sorting_context =
      !sorting_context_id && !transform.IsFlat();

  if (!clip_and_sorting_layers.empty()) {
    ClipAndSortingLayer& current_layer = clip_and_sorting_layers.back();
    // It is in error to change the clipping settings within a non-zero sorting
    // context. The result will be incorrect layering and intersection.
    if (sorting_context_id &&
        current_layer.sorting_context_id == sorting_context_id &&
        (current_layer.is_clipped != is_clipped ||
         current_layer.clip_rect != clip_rect)) {
      // Excessive logging to debug white screens (crbug.com/583805).
      // TODO(ccameron): change this back to a DLOG.
      LOG(ERROR) << "CALayer changed clip inside non-zero sorting context.";
      return false;
    }
    if (!is_singleton_sorting_context &&
        !current_layer.is_singleton_sorting_context &&
        current_layer.is_clipped == is_clipped &&
        current_layer.clip_rect == clip_rect &&
        current_layer.sorting_context_id == sorting_context_id) {
      needs_new_clip_and_sorting_layer = false;
    }
  }
  if (needs_new_clip_and_sorting_layer) {
    clip_and_sorting_layers.push_back(
        ClipAndSortingLayer(is_clipped, clip_rect, sorting_context_id,
                            is_singleton_sorting_context));
  }
  clip_and_sorting_layers.back().AddContentLayer(
      transform, io_surface, contents_rect, rect, background_color,
      edge_aa_mask, opacity);
  return true;
}

void CALayerTree::ClipAndSortingLayer::AddContentLayer(
    const gfx::Transform& transform,
    base::ScopedCFTypeRef<IOSurfaceRef> io_surface,
    const gfx::RectF& contents_rect,
    const gfx::Rect& rect,
    unsigned background_color,
    unsigned edge_aa_mask,
    float opacity) {
  bool needs_new_transform_layer = true;
  if (!transform_layers.empty()) {
    const TransformLayer& current_layer = transform_layers.back();
    if (current_layer.transform == transform)
      needs_new_transform_layer = false;
  }
  if (needs_new_transform_layer)
    transform_layers.push_back(TransformLayer(transform));
  transform_layers.back().AddContentLayer(
      io_surface, contents_rect, rect, background_color, edge_aa_mask, opacity);
}

void CALayerTree::TransformLayer::AddContentLayer(
    base::ScopedCFTypeRef<IOSurfaceRef> io_surface,
    const gfx::RectF& contents_rect,
    const gfx::Rect& rect,
    unsigned background_color,
    unsigned edge_aa_mask,
    float opacity) {
  content_layers.push_back(ContentLayer(io_surface, contents_rect, rect,
                                        background_color, edge_aa_mask,
                                        opacity));
}

void CALayerTree::RootLayer::CommitToCA(CALayer* superlayer,
                                        RootLayer* old_layer,
                                        float scale_factor) {
  if (old_layer) {
    DCHECK(old_layer->ca_layer);
    std::swap(ca_layer, old_layer->ca_layer);
  } else {
    ca_layer.reset([[CALayer alloc] init]);
    [ca_layer setAnchorPoint:CGPointZero];
    [superlayer setSublayers:nil];
    [superlayer addSublayer:ca_layer];
    [superlayer setBorderWidth:0];
  }
  // Excessive logging to debug white screens (crbug.com/583805).
  // TODO(ccameron): change this back to a DCHECK.
  if ([ca_layer superlayer] != superlayer) {
    LOG(ERROR) << "CALayerTree root layer not attached to tree.";
  }

  for (size_t i = 0; i < clip_and_sorting_layers.size(); ++i) {
    ClipAndSortingLayer* old_clip_and_sorting_layer = nullptr;
    if (old_layer && i < old_layer->clip_and_sorting_layers.size()) {
      old_clip_and_sorting_layer = &old_layer->clip_and_sorting_layers[i];
    }
    clip_and_sorting_layers[i].CommitToCA(
        ca_layer.get(), old_clip_and_sorting_layer, scale_factor);
  }
}

void CALayerTree::ClipAndSortingLayer::CommitToCA(
    CALayer* superlayer,
    ClipAndSortingLayer* old_layer,
    float scale_factor) {
  bool update_is_clipped = true;
  bool update_clip_rect = true;
  if (old_layer) {
    DCHECK(old_layer->ca_layer);
    std::swap(ca_layer, old_layer->ca_layer);
    update_is_clipped = old_layer->is_clipped != is_clipped;
    update_clip_rect = old_layer->clip_rect != clip_rect;
  } else {
    ca_layer.reset([[CALayer alloc] init]);
    [ca_layer setAnchorPoint:CGPointZero];
    [superlayer addSublayer:ca_layer];
  }
  // Excessive logging to debug white screens (crbug.com/583805).
  // TODO(ccameron): change this back to a DCHECK.
  if ([ca_layer superlayer] != superlayer) {
    LOG(ERROR) << "CALayerTree root layer not attached to tree.";
  }

  if (update_is_clipped)
    [ca_layer setMasksToBounds:is_clipped];

  if (update_clip_rect) {
    if (is_clipped) {
      gfx::RectF dip_clip_rect = gfx::RectF(clip_rect);
      dip_clip_rect.Scale(1 / scale_factor);
      [ca_layer setPosition:CGPointMake(dip_clip_rect.x(), dip_clip_rect.y())];
      [ca_layer setBounds:CGRectMake(0, 0, dip_clip_rect.width(),
                                     dip_clip_rect.height())];
      [ca_layer
          setSublayerTransform:CATransform3DMakeTranslation(
                                   -dip_clip_rect.x(), -dip_clip_rect.y(), 0)];
    } else {
      [ca_layer setPosition:CGPointZero];
      [ca_layer setBounds:CGRectZero];
      [ca_layer setSublayerTransform:CATransform3DIdentity];
    }
  }

  for (size_t i = 0; i < transform_layers.size(); ++i) {
    TransformLayer* old_transform_layer = nullptr;
    if (old_layer && i < old_layer->transform_layers.size())
      old_transform_layer = &old_layer->transform_layers[i];
    transform_layers[i].CommitToCA(ca_layer.get(), old_transform_layer,
                                   scale_factor);
  }
}

void CALayerTree::TransformLayer::CommitToCA(CALayer* superlayer,
                                             TransformLayer* old_layer,
                                             float scale_factor) {
  bool update_transform = true;
  if (old_layer) {
    DCHECK(old_layer->ca_layer);
    std::swap(ca_layer, old_layer->ca_layer);
    update_transform = old_layer->transform != transform;
  } else {
    ca_layer.reset([[CATransformLayer alloc] init]);
    [superlayer addSublayer:ca_layer];
  }
  DCHECK_EQ([ca_layer superlayer], superlayer);

  if (update_transform) {
    gfx::Transform pre_scale;
    gfx::Transform post_scale;
    pre_scale.Scale(1 / scale_factor, 1 / scale_factor);
    post_scale.Scale(scale_factor, scale_factor);
    gfx::Transform conjugated_transform = pre_scale * transform * post_scale;

    CATransform3D ca_transform;
    conjugated_transform.matrix().asColMajord(&ca_transform.m11);
    [ca_layer setTransform:ca_transform];
  }

  for (size_t i = 0; i < content_layers.size(); ++i) {
    ContentLayer* old_content_layer = nullptr;
    if (old_layer && i < old_layer->content_layers.size())
      old_content_layer = &old_layer->content_layers[i];
    content_layers[i].CommitToCA(ca_layer.get(), old_content_layer,
                                 scale_factor);
  }
}

void CALayerTree::ContentLayer::CommitToCA(CALayer* superlayer,
                                           ContentLayer* old_layer,
                                           float scale_factor) {
  bool update_contents = true;
  bool update_contents_rect = true;
  bool update_rect = true;
  bool update_background_color = true;
  bool update_ca_edge_aa_mask = true;
  bool update_opacity = true;
  if (old_layer) {
    DCHECK(old_layer->ca_layer);
    std::swap(ca_layer, old_layer->ca_layer);
    update_contents = old_layer->io_surface != io_surface;
    update_contents_rect = old_layer->contents_rect != contents_rect;
    update_rect = old_layer->rect != rect;
    update_background_color = old_layer->background_color != background_color;
    update_ca_edge_aa_mask = old_layer->ca_edge_aa_mask != ca_edge_aa_mask;
    update_opacity = old_layer->opacity != opacity;
  } else {
    ca_layer.reset([[CALayer alloc] init]);
    [ca_layer setAnchorPoint:CGPointZero];
    [superlayer addSublayer:ca_layer];
  }
  DCHECK_EQ([ca_layer superlayer], superlayer);
  bool update_anything = update_contents || update_contents_rect ||
                         update_rect || update_background_color ||
                         update_ca_edge_aa_mask || update_opacity;

  if (update_contents) {
    [ca_layer setContents:(__bridge id)(io_surface.get())];
    if ([ca_layer respondsToSelector:(@selector(setContentsScale:))])
      [ca_layer setContentsScale:scale_factor];
  }
  if (update_contents_rect)
    [ca_layer setContentsRect:contents_rect.ToCGRect()];
  if (update_rect) {
    gfx::RectF dip_rect = gfx::RectF(rect);
    dip_rect.Scale(1 / scale_factor);
    [ca_layer setPosition:CGPointMake(dip_rect.x(), dip_rect.y())];
    [ca_layer setBounds:CGRectMake(0, 0, dip_rect.width(), dip_rect.height())];
  }
  if (update_background_color) {
    CGFloat rgba_color_components[4] = {
        SkColorGetR(background_color) / 255.,
        SkColorGetG(background_color) / 255.,
        SkColorGetB(background_color) / 255.,
        SkColorGetA(background_color) / 255.,
    };
    base::ScopedCFTypeRef<CGColorRef> srgb_background_color(CGColorCreate(
        CGColorSpaceCreateWithName(kCGColorSpaceSRGB), rgba_color_components));
    [ca_layer setBackgroundColor:srgb_background_color];
  }
  if (update_ca_edge_aa_mask)
    [ca_layer setEdgeAntialiasingMask:ca_edge_aa_mask];
  if (update_opacity)
    [ca_layer setOpacity:opacity];

  static bool show_borders = base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kShowMacOverlayBorders);
  if (show_borders) {
    base::ScopedCFTypeRef<CGColorRef> color;
    if (update_anything) {
      // Pink represents a CALayer that changed this frame.
      color.reset(CGColorCreateGenericRGB(1, 0, 1, 1));
    } else {
      // Grey represents a CALayer that has not changed.
      color.reset(CGColorCreateGenericRGB(0, 0, 0, 0.1));
    }
    [ca_layer setBorderWidth:1];
    [ca_layer setBorderColor:color];
  }
}

}  // namespace content
