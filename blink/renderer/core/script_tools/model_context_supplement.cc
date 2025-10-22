// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/script_tools/model_context_supplement.h"

#include "third_party/blink/renderer/core/frame/navigator.h"

namespace blink {

// static
const char ModelContextSupplement::kSupplementName[] = "ModelContextSupplement";

// static
ModelContextSupplement& ModelContextSupplement::From(Navigator& navigator) {
  ModelContextSupplement* supplement =
      Supplement<Navigator>::From<ModelContextSupplement>(navigator);
  if (!supplement) {
    supplement = MakeGarbageCollected<ModelContextSupplement>(navigator);
    ProvideTo(navigator, supplement);
  }
  return *supplement;
}

// static
ModelContext* ModelContextSupplement::GetIfExists(Navigator& navigator) {
  ModelContextSupplement* supplement =
      Supplement<Navigator>::From<ModelContextSupplement>(navigator);
  return supplement ? supplement->modelContext() : nullptr;
}

// static
ModelContext* ModelContextSupplement::modelContext(Navigator& navigator) {
  return From(navigator).modelContext();
}

ModelContextSupplement::ModelContextSupplement(Navigator& navigator)
    : Supplement<Navigator>(navigator) {}

void ModelContextSupplement::Trace(Visitor* visitor) const {
  visitor->Trace(model_context_);
  Supplement<Navigator>::Trace(visitor);
}

ModelContext* ModelContextSupplement::modelContext() {
  if (!model_context_) {
    if (auto* window = GetSupplementable()->DomWindow()) {
      model_context_ = MakeGarbageCollected<ModelContext>(
          window->GetTaskRunner(TaskType::kUserInteraction));
    }
  }
  return model_context_.Get();
}

}  // namespace blink
