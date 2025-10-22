// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_COMPOSITION_LAYER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_COMPOSITION_LAYER_H_

#include <optional>

#include "device/vr/public/mojom/vr_service.mojom-blink.h"
#include "third_party/blink/renderer/bindings/modules/v8/v8_xr_layer_layout.h"
#include "third_party/blink/renderer/modules/xr/xr_layer.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class V8XRLayerLayout;
class XRGraphicsBinding;
class XRLayerDrawingContext;

class XRCompositionLayer : public XRLayer {
  DEFINE_WRAPPERTYPEINFO();

 public:
  XRCompositionLayer(XRGraphicsBinding* binding,
                     XRLayerDrawingContext* drawing_context);
  ~XRCompositionLayer() override = default;

  XRGraphicsBinding* binding() const { return binding_.Get(); }
  V8XRLayerLayout layout() const;
  bool blendTextureSourceAlpha() const;
  void setBlendTextureSourceAlpha(bool value);
  std::optional<bool> chromaticAberrationCorrection() const;
  void setChromaticAberrationCorrection(std::optional<bool> value);
  bool forceMonoPresentation() const;
  void setForceMonoPresentation(bool value);
  float opacity() const;
  void setOpacity(float value);
  uint16_t mipLevels() const;
  bool needsRedraw() const;
  void destroy() const;

  uint16_t textureWidth() const;
  uint16_t textureHeight() const;
  uint16_t textureArrayLength() const;

  void OnFrameStart() override;
  void OnFrameEnd() override;
  void OnResize() override {}

  XRLayerDrawingContext* drawing_context() { return drawing_context_; }

  XrLayerClient* LayerClient() override;

  void Trace(Visitor*) const override;

 protected:
  void SetNeedsRedraw(bool needsRedraw);
  void SetLayout(V8XRLayerLayout layout);
  void SetMipLevels(uint16_t mipLevels);

 private:
  V8XRLayerLayout::Enum layout_ = V8XRLayerLayout::Enum::kDefault;

  const Member<XRGraphicsBinding> binding_;
  bool blend_texture_source_alpha_{false};
  std::optional<bool> chromatic_aberration_correction_{std::nullopt};
  bool force_mono_presentation_{false};
  float opacity_{1.0};
  uint16_t mip_levels_{1};
  bool needs_redraw_{false};

  Member<XRLayerDrawingContext> drawing_context_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_XR_XR_COMPOSITION_LAYER_H_
