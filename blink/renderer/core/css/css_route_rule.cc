// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/css_route_rule.h"

#include "third_party/blink/renderer/core/css/style_rule.h"

namespace blink {

CSSRouteRule::CSSRouteRule(StyleRuleRoute* route_rule, CSSStyleSheet* parent)
    : CSSConditionRule(route_rule, parent), route_rule_(route_rule) {}

CSSRouteRule::~CSSRouteRule() = default;

String CSSRouteRule::cssText() const {
  // TODO(crbug.com/436805487): Implement this.
  NOTREACHED() << "Not yet implemented.";
}

void CSSRouteRule::Reattach(StyleRuleBase* rule) {
  DCHECK(rule);
  route_rule_ = To<StyleRuleRoute>(rule);
  CSSConditionRule::Reattach(rule);
}

void CSSRouteRule::Trace(Visitor* visitor) const {
  visitor->Trace(route_rule_);
  CSSConditionRule::Trace(visitor);
}

}  // namespace blink
