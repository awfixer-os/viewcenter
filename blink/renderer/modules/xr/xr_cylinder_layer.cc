// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/xr/xr_cylinder_layer.h"

#include "third_party/blink/renderer/bindings/modules/v8/v8_xr_cylinder_layer_init.h"
#include "third_party/blink/renderer/modules/xr/xr_rigid_transform.h"
#include "third_party/blink/renderer/modules/xr/xr_session.h"

namespace blink {
XRCylinderLayer::XRCylinderLayer(const XRCylinderLayerInit* init,
                                 XRGraphicsBinding* binding,
                                 XRLayerDrawingContext* drawing_context)
    : XRShapedLayer(init, binding, drawing_context),
      radius_(init->radius()),
      central_angle_(init->centralAngle()),
      aspect_ratio_(init->aspectRatio()) {
  if (init->hasTransform()) {
    transform_ = MakeGarbageCollected<XRRigidTransform>(
        init->transform()->TransformMatrix());
  } else {
    transform_ = MakeGarbageCollected<XRRigidTransform>(gfx::Transform{});
  }
}

XRLayerType XRCylinderLayer::LayerType() const {
  return XRLayerType::kCylinderLayer;
}

void XRCylinderLayer::setRadius(float radius) {
  radius_ = radius;
  SetModified(true);
}

void XRCylinderLayer::setCentralAngle(float central_angle) {
  central_angle_ = central_angle;
  SetModified(true);
}

void XRCylinderLayer::setAspectRatio(float aspect_ratio) {
  aspect_ratio_ = aspect_ratio;
  SetModified(true);
}

void XRCylinderLayer::setTransform(XRRigidTransform* value) {
  if (transform_ != value) {
    transform_ = value;
    SetModified(true);
  }
}

void XRCylinderLayer::Trace(Visitor* visitor) const {
  visitor->Trace(transform_);
  XRShapedLayer::Trace(visitor);
}

}  // namespace blink
