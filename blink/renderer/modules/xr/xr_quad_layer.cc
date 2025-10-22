// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/xr/xr_quad_layer.h"

#include "device/vr/public/mojom/vr_service.mojom-blink.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_xr_quad_layer_init.h"
#include "third_party/blink/renderer/core/typed_arrays/dom_typed_array.h"
#include "third_party/blink/renderer/modules/xr/xr_graphics_binding.h"
#include "third_party/blink/renderer/modules/xr/xr_rigid_transform.h"
#include "third_party/blink/renderer/modules/xr/xr_session.h"

namespace blink {

XRQuadLayer::XRQuadLayer(const XRQuadLayerInit* init,
                         XRGraphicsBinding* binding,
                         XRLayerDrawingContext* drawing_context)
    : XRShapedLayer(init, binding, drawing_context),
      width_(init->width()),
      height_(init->height()) {
  if (init->hasTransform()) {
    transform_ = MakeGarbageCollected<XRRigidTransform>(
        init->transform()->TransformMatrix());
  } else {
    transform_ = MakeGarbageCollected<XRRigidTransform>(gfx::Transform{});
  }
}

XRLayerType XRQuadLayer::LayerType() const {
  return XRLayerType::kQuadLayer;
}

void XRQuadLayer::setWidth(float width) {
  width_ = width;
  SetModified(true);
}

void XRQuadLayer::setHeight(float height) {
  height_ = height;
  SetModified(true);
}

void XRQuadLayer::setTransform(XRRigidTransform* value) {
  if (transform_ != value) {
    transform_ = value;
    SetModified(true);
  }
}

void XRQuadLayer::Trace(Visitor* visitor) const {
  visitor->Trace(transform_);
  XRShapedLayer::Trace(visitor);
}

}  // namespace blink
