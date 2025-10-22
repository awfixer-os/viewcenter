// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_LAYER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_LAYER_H_

#include <optional>

#include "device/vr/public/mojom/layer_id.h"
#include "gpu/command_buffer/common/mailbox_holder.h"
#include "third_party/blink/renderer/core/dom/events/event_target.h"

namespace blink {

class XrLayerClient;
class XRSession;
struct XRSharedImageData;

enum class XRLayerType {
  kWebGLLayer,
  kProjectionLayer,
  kQuadLayer,
  kCylinderLayer,
  kEquirectLayer
};

class XRLayer : public EventTarget {
  DEFINE_WRAPPERTYPEINFO();

 public:
  explicit XRLayer(XRSession*);
  ~XRLayer() override = default;

  XRSession* session() const { return session_.Get(); }

  virtual void OnFrameStart() = 0;
  virtual void OnFrameEnd() = 0;
  virtual void OnResize() = 0;

  // EventTarget overrides.
  ExecutionContext* GetExecutionContext() const override;
  const AtomicString& InterfaceName() const override;

  device::LayerId layer_id() const { return layer_id_; }
  virtual XRLayerType LayerType() const = 0;

  const XRSharedImageData& SharedImage() const;
  bool HasSharedImage() const;

  void SetModified(bool modified);
  bool IsModified() const;

  virtual XrLayerClient* LayerClient() = 0;

  void Trace(Visitor*) const override;

 private:
  const Member<XRSession> session_;
  const device::LayerId layer_id_;
  bool is_modified_{false};
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_LAYER_H_
