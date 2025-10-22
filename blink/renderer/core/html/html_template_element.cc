/*
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/core/html/html_template_element.h"

#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/document_fragment.h"
#include "third_party/blink/renderer/core/dom/mutation_observer.h"
#include "third_party/blink/renderer/core/dom/node_cloning_data.h"
#include "third_party/blink/renderer/core/dom/template_content_document_fragment.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/patching/patch.h"
#include "third_party/blink/renderer/core/patching/patch_supplement.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"

namespace blink {

HTMLTemplateElement::HTMLTemplateElement(Document& document)
    : HTMLElement(html_names::kTemplateTag, document) {
  UseCounter::Count(document, WebFeature::kHTMLTemplateElement);
}

HTMLTemplateElement::~HTMLTemplateElement() = default;

DocumentFragment* HTMLTemplateElement::content() const {
  CHECK(!override_insertion_target_);
  if (!content_ && GetExecutionContext())
    content_ = MakeGarbageCollected<TemplateContentDocumentFragment>(
        GetDocument().EnsureTemplateDocument(),
        const_cast<HTMLTemplateElement*>(this));

  return content_.Get();
}

// https://html.spec.whatwg.org/C/#the-template-element:concept-node-clone-ext
void HTMLTemplateElement::CloneNonAttributePropertiesFrom(
    const Element& source,
    NodeCloningData& data) {
  if (!data.Has(CloneOption::kIncludeDescendants) || !GetExecutionContext()) {
    return;
  }
  auto& html_template_element = To<HTMLTemplateElement>(source);
  if (html_template_element.content()) {
    content()->CloneChildNodesFrom(*html_template_element.content(), data,
                                   /*fallback_registry*/ nullptr);
  }
}

void HTMLTemplateElement::DidMoveToNewDocument(Document& old_document) {
  HTMLElement::DidMoveToNewDocument(old_document);
  if (!content_ || !GetExecutionContext())
    return;
  GetDocument().EnsureTemplateDocument().AdoptIfNeeded(*content_);
}

void HTMLTemplateElement::Trace(Visitor* visitor) const {
  visitor->Trace(content_);
  visitor->Trace(override_insertion_target_);
  visitor->Trace(patch_status_);
  HTMLElement::Trace(visitor);
}

bool HTMLTemplateElement::ProcessPatch(ContainerNode& target) {
  // We can't use GetElementAttribute here because the template is not attached
  // to the DOM.
  Element* start_after = FastHasAttribute(html_names::kPatchstartafterAttr)
                             ? target.getElementById(FastGetAttribute(
                                   html_names::kPatchstartafterAttr))
                             : nullptr;
  Element* end_before = FastHasAttribute(html_names::kPatchendbeforeAttr)
                            ? target.getElementById(FastGetAttribute(
                                  html_names::kPatchendbeforeAttr))
                            : nullptr;
  if ((start_after && start_after->parentElement() != &target) ||
      (end_before && end_before->parentElement() != &target)) {
    // TODO(nrosenthal): fire a patcherror event?
    return false;
  }

  const KURL src = FastHasAttribute(html_names::kPatchsrcAttr)
                       ? target.GetDocument().CompleteURL(
                             FastGetAttribute(html_names::kPatchsrcAttr))
                       : KURL();
  SetOverrideInsertionTarget(target);
  patch_status_ = Patch::Create(target, this, src, start_after, end_before);
  patch_status_->Start();
  return true;
}

void HTMLTemplateElement::FinishParsingChildren() {
  HTMLElement::FinishParsingChildren();
  if (!patch_status_) {
    return;
  }
  CHECK(RuntimeEnabledFeatures::DocumentPatchingEnabled());
  patch_status_->Finish();
  patch_status_.Release();
}

}  // namespace blink
