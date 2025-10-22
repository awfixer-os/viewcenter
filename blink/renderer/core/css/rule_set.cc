/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 2004-2005 Allan Sandfeld Jensen (kde@carewolf.com)
 * Copyright (C) 2006, 2007 Nicholas Shanks (webkit@nickshanks.com)
 * Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012 Apple Inc. All
 * rights reserved.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 * Copyright (C) 2007, 2008 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved.
 * (http://www.torchmobile.com/)
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 * Copyright (C) 2012 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/core/css/rule_set.h"

#include <memory>
#include <type_traits>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/contains.h"
#include "base/substring_set_matcher/substring_set_matcher.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/renderer/core/css/css_font_selector.h"
#include "third_party/blink/renderer/core/css/css_selector.h"
#include "third_party/blink/renderer/core/css/css_selector_list.h"
#include "third_party/blink/renderer/core/css/media_values.h"
#include "third_party/blink/renderer/core/css/robin_hood_map-inl.h"
#include "third_party/blink/renderer/core/css/seeker.h"
#include "third_party/blink/renderer/core/css/selector_checker-inl.h"
#include "third_party/blink/renderer/core/css/selector_checker.h"
#include "third_party/blink/renderer/core/css/selector_filter.h"
#include "third_party/blink/renderer/core/css/style_rule_import.h"
#include "third_party/blink/renderer/core/css/style_rule_nested_declarations.h"
#include "third_party/blink/renderer/core/css/style_sheet_contents.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/html/shadow/shadow_element_names.h"
#include "third_party/blink/renderer/core/html/shadow/shadow_element_utils.h"
#include "third_party/blink/renderer/core/html/track/text_track_cue.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/inspector/invalidation_set_to_selector_map.h"
#include "third_party/blink/renderer/core/route_matching/route_map.h"
#include "third_party/blink/renderer/core/style/computed_style_constants.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/trace_event.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"

using base::MatcherStringPattern;
using base::SubstringSetMatcher;

namespace blink {

template <class T>
static void AddRuleToIntervals(const T* value,
                               unsigned position,
                               HeapVector<RuleSet::Interval<T>>& intervals);

static void UnmarkAsCoveredByBucketing(CSSSelector& selector);

static inline ValidPropertyFilter DetermineValidPropertyFilter(
    const AddRuleFlags add_rule_flags,
    const CSSSelector& selector) {
  for (const CSSSelector* component = &selector; component;
       component = component->NextSimpleSelector()) {
    if (component->Match() == CSSSelector::kPseudoElement &&
        component->Value() == TextTrackCue::CueShadowPseudoId()) {
      return ValidPropertyFilter::kCue;
    }
    switch (component->GetPseudoType()) {
      case CSSSelector::kPseudoCue:
        return ValidPropertyFilter::kCue;
      case CSSSelector::kPseudoFirstLetter:
        return ValidPropertyFilter::kFirstLetter;
      case CSSSelector::kPseudoFirstLine:
        return ValidPropertyFilter::kFirstLine;
      case CSSSelector::kPseudoMarker:
        return ValidPropertyFilter::kMarker;
      case CSSSelector::kPseudoSelection:
      case CSSSelector::kPseudoTargetText:
      case CSSSelector::kPseudoGrammarError:
      case CSSSelector::kPseudoSpellingError:
      case CSSSelector::kPseudoHighlight:
      case CSSSelector::kPseudoSearchText:
        return ValidPropertyFilter::kHighlight;
      default:
        break;
    }
  }
  return ValidPropertyFilter::kNoFilter;
}

static bool SelectorListHasLinkOrVisited(const CSSSelector* selector_list) {
  for (const CSSSelector* complex = selector_list; complex;
       complex = CSSSelectorList::Next(*complex)) {
    if (complex->HasLinkOrVisited()) {
      return true;
    }
  }
  return false;
}

static bool StyleScopeHasLinkOrVisited(const StyleScope* style_scope) {
  return style_scope && (SelectorListHasLinkOrVisited(style_scope->From()) ||
                         SelectorListHasLinkOrVisited(style_scope->To()));
}

static unsigned DetermineLinkMatchType(const AddRuleFlags add_rule_flags,
                                       const CSSSelector& selector,
                                       const StyleScope* style_scope) {
  if (selector.HasLinkOrVisited() || StyleScopeHasLinkOrVisited(style_scope)) {
    return (add_rule_flags & kRuleIsVisitedDependent)
               ? CSSSelector::kMatchVisited
               : CSSSelector::kMatchLink;
  }
  return CSSSelector::kMatchAll;
}

RuleData::RuleData(StyleRule* rule,
                   unsigned selector_index,
                   unsigned position,
                   const StyleScope* style_scope,
                   AddRuleFlags add_rule_flags,
                   Vector<uint16_t>& bloom_hash_backing)
    : rule_(rule),
      selector_index_(selector_index),
      position_(position),
      specificity_(Selector().Specificity()),
      link_match_type_(
          DetermineLinkMatchType(add_rule_flags, Selector(), style_scope)),
      valid_property_filter_(
          static_cast<std::underlying_type_t<ValidPropertyFilter>>(
              DetermineValidPropertyFilter(add_rule_flags, Selector()))),
      is_entirely_covered_by_bucketing_(
          false),  // Will be computed in ComputeEntirelyCoveredByBucketing().
      is_easy_(false),  // Ditto.
      is_starting_style_((add_rule_flags & kRuleIsStartingStyle) != 0),
      bloom_hash_size_(0),
      bloom_hash_pos_(0) {
  ComputeBloomFilterHashes(style_scope, bloom_hash_backing);
}

void RuleData::ComputeEntirelyCoveredByBucketing() {
  is_easy_ = EasySelectorChecker::IsEasy(&Selector());
  is_entirely_covered_by_bucketing_ = true;
  for (const CSSSelector* selector = &Selector(); selector;
       selector = selector->NextSimpleSelector()) {
    if (!selector->IsCoveredByBucketing()) {
      is_entirely_covered_by_bucketing_ = false;
      break;
    }
  }
}

void RuleData::ResetEntirelyCoveredByBucketing() {
  for (CSSSelector* selector = &MutableSelector(); selector;
       selector = selector->NextSimpleSelector()) {
    selector->SetCoveredByBucketing(false);
    if (selector->Relation() != CSSSelector::kSubSelector) {
      break;
    }
  }
  is_entirely_covered_by_bucketing_ = false;
}

void RuleData::ComputeBloomFilterHashes(const StyleScope* style_scope,
                                        Vector<uint16_t>& bloom_hash_backing) {
  if (bloom_hash_backing.size() >= 16777216) {
    // This won't fit into bloom_hash_pos_, so don't collect any hashes.
    return;
  }
  bloom_hash_pos_ = bloom_hash_backing.size();
  SelectorFilter::CollectIdentifierHashes(Selector(), style_scope,
                                          bloom_hash_backing, subject_filter_);

  // The clamp here is purely for safety; a real rule would never have
  // as many as 255 descendant selectors.
  bloom_hash_size_ =
      std::min<uint32_t>(bloom_hash_backing.size() - bloom_hash_pos_, 255);

  // If we've already got the exact same set of hashes in the vector,
  // we can simply reuse those, saving a bit of memory and cache space.
  // We only check the trivial case of a tail match; we could go with
  // something like a full suffix tree solution, but this is simple and
  // captures most of the benefits. (It is fairly common, especially with
  // nesting, to have the same sets of parents in consecutive rules.)
  if (bloom_hash_size_ > 0 && bloom_hash_pos_ >= bloom_hash_size_ &&
      UNSAFE_TODO(std::equal(
          bloom_hash_backing.begin() + bloom_hash_pos_ - bloom_hash_size_,
          bloom_hash_backing.begin() + bloom_hash_pos_,
          bloom_hash_backing.begin() + bloom_hash_pos_))) {
    bloom_hash_backing.resize(bloom_hash_pos_);
    bloom_hash_pos_ -= bloom_hash_size_;
  }
}

void RuleData::MovedToDifferentRuleSet(const Vector<uint16_t>& old_backing,
                                       Vector<uint16_t>& new_backing,
                                       unsigned new_position) {
  unsigned new_pos = new_backing.size();
  new_backing.insert(new_backing.size(),
                     UNSAFE_TODO(old_backing.data() + bloom_hash_pos_),
                     bloom_hash_size_);
  bloom_hash_pos_ = new_pos;
  position_ = new_position;
}

void RuleSet::AddToBucket(const AtomicString& key,
                          RuleMap& map,
                          const RuleData& rule_data) {
  if (map.IsCompacted()) {
    // This normally should not happen, but may with UA stylesheets;
    // see class comment on RuleMap.
    map.Uncompact();
  }
  if (!map.Add(key, rule_data)) {
    // This should really only happen in case of an attack;
    // we stick it in the universal bucket so that correctness
    // is preserved, even though the performance will be suboptimal.
    RuleData rule_data_copy = rule_data;
    UnmarkAsCoveredByBucketing(rule_data_copy.MutableSelector());
    AddToBucket(universal_rules_, rule_data_copy);
    return;
  }
  // Don't call ComputeBloomFilterHashes() here; RuleMap needs that space for
  // group information, and will call ComputeBloomFilterHashes() itself on
  // compaction.
  need_compaction_ = true;
}

void RuleSet::AddToBucket(HeapVector<RuleData>& rules,
                          const RuleData& rule_data) {
  rules.push_back(rule_data);
  rules.back().ComputeEntirelyCoveredByBucketing();
  need_compaction_ = true;
}

namespace {

// Pseudo-elements that should stop extracting bucketing information
// from selector after themselves, as they allow some pseudo-classes after them
// in selector, which can confuse bucketing (for now, if you have to add a new
// PseudoType here, the rule is: if it creates a PseudoElement object - return
// true, otherwise - return false).
bool ShouldStopExtractingAtPseudoElement(
    const CSSSelector::PseudoType& pseudo_type) {
  switch (pseudo_type) {
    case CSSSelector::kPseudoCheckMark:
    case CSSSelector::kPseudoPickerIcon:
    case CSSSelector::kPseudoFirstLetter:
    case CSSSelector::kPseudoScrollButton:
    case CSSSelector::kPseudoScrollMarker:
    case CSSSelector::kPseudoAfter:
    case CSSSelector::kPseudoBefore:
    case CSSSelector::kPseudoInterestHint:
    case CSSSelector::kPseudoBackdrop:
    case CSSSelector::kPseudoMarker:
    case CSSSelector::kPseudoColumn:
    case CSSSelector::kPseudoViewTransition:
    case CSSSelector::kPseudoViewTransitionGroup:
    case CSSSelector::kPseudoViewTransitionGroupChildren:
    case CSSSelector::kPseudoViewTransitionImagePair:
    case CSSSelector::kPseudoViewTransitionNew:
    case CSSSelector::kPseudoViewTransitionOld:
    case CSSSelector::kPseudoScrollMarkerGroup:
      return true;
    case CSSSelector::kPseudoCue:
    case CSSSelector::kPseudoFirstLine:
    case CSSSelector::kPseudoSelection:
    case CSSSelector::kPseudoScrollbar:
    case CSSSelector::kPseudoScrollbarButton:
    case CSSSelector::kPseudoScrollbarCorner:
    case CSSSelector::kPseudoScrollbarThumb:
    case CSSSelector::kPseudoScrollbarTrack:
    case CSSSelector::kPseudoScrollbarTrackPiece:
    case CSSSelector::kPseudoSlotted:
    case CSSSelector::kPseudoPart:
    case CSSSelector::kPseudoResizer:
    case CSSSelector::kPseudoSearchText:
    case CSSSelector::kPseudoTargetText:
    case CSSSelector::kPseudoHighlight:
    case CSSSelector::kPseudoSpellingError:
    case CSSSelector::kPseudoGrammarError:
    case CSSSelector::kPseudoPlaceholder:
    case CSSSelector::kPseudoFileSelectorButton:
    case CSSSelector::kPseudoDetailsContent:
    case CSSSelector::kPseudoPermissionIcon:
    case CSSSelector::kPseudoPicker:
    case CSSSelector::kPseudoWebKitCustomElement:
    case CSSSelector::kPseudoBlinkInternalElement:
      return false;
    default:
      NOTREACHED()
          << "Don't forget to add new pseudo-element type in this switch "
          << static_cast<wtf_size_t>(pseudo_type);
  }
}

// A collection of values that determine which bucket a given rule goes into.
//
// See FindBestBucketAndAdd.
struct BucketingValues {
  STACK_ALLOCATED();

 public:
  AtomicString id;
  AtomicString class_name;
  AtomicString attr_name;
  AtomicString attr_value;
  bool is_exact_attr = false;
  AtomicString custom_pseudo_element_name;
  AtomicString tag_name;
  AtomicString part_name;
  AtomicString ua_shadow_pseudo;
  CSSSelector::PseudoType pseudo_type = CSSSelector::kPseudoUnknown;
  bool has_slotted = false;
};

}  // namespace

// The return value indicates if extracting can continue
// or should be stopped due to reaching some pseudo-element
// that doesn't allow extracting bucketing rules after itself
// in selector.
static bool ExtractBucketingValues(const CSSSelector* selector,
                                   const StyleScope* style_scope,
                                   BucketingValues& values) {
  switch (selector->Match()) {
    case CSSSelector::kId:
      values.id = selector->Value();
      break;
    case CSSSelector::kClass:
      values.class_name = selector->Value();
      break;
    case CSSSelector::kTag:
      values.tag_name = selector->TagQName().LocalName();
      break;
    case CSSSelector::kPseudoElement:
      // TODO(403505399): We shouldn't allow bucketing of pseudo-classes
      // after pseudo-elements for now, as it confuses bucketing.
      if (ShouldStopExtractingAtPseudoElement(selector->GetPseudoType())) {
        return false;
      }
      [[fallthrough]];
    case CSSSelector::kPseudoClass:
    case CSSSelector::kPagePseudoClass:
      // Must match the cases in RuleSet::FindBestBucketAndAdd.
      switch (selector->GetPseudoType()) {
        case CSSSelector::kPseudoFocus:
        case CSSSelector::kPseudoCue:
        case CSSSelector::kPseudoLink:
        case CSSSelector::kPseudoVisited:
        case CSSSelector::kPseudoWebkitAnyLink:
        case CSSSelector::kPseudoAnyLink:
        case CSSSelector::kPseudoFocusVisible:
        case CSSSelector::kPseudoHost:
        case CSSSelector::kPseudoHostContext:
        case CSSSelector::kPseudoSlotted:
        case CSSSelector::kPseudoSelectorFragmentAnchor:
        case CSSSelector::kPseudoRoot:
        case CSSSelector::kPseudoActiveViewTransition:
          // Pseudo classes.
          values.pseudo_type = selector->GetPseudoType();
          if (values.pseudo_type == CSSSelector::kPseudoSlotted) {
            values.has_slotted = true;
          }
          break;
        case CSSSelector::kPseudoPlaceholder:
        case CSSSelector::kPseudoDetailsContent:
        case CSSSelector::kPseudoPermissionIcon:
        case CSSSelector::kPseudoFileSelectorButton:
        case CSSSelector::kPseudoScrollbarButton:
        case CSSSelector::kPseudoScrollbarCorner:
        case CSSSelector::kPseudoScrollbarThumb:
        case CSSSelector::kPseudoScrollbarTrack:
        case CSSSelector::kPseudoScrollbarTrackPiece:
          // Pseudo elements; do not overwrite a pseudo class
          // (in particular, :host).
          if (values.pseudo_type == CSSSelector::kPseudoUnknown) {
            values.pseudo_type = selector->GetPseudoType();
            values.ua_shadow_pseudo =
                shadow_element_utils::StringForUAShadowPseudoId(
                    CSSSelector::GetPseudoId(values.pseudo_type));
          }
          break;
        case CSSSelector::kPseudoWebKitCustomElement:
        case CSSSelector::kPseudoBlinkInternalElement:
          values.custom_pseudo_element_name = selector->Value();
          break;
        case CSSSelector::kPseudoPart:
          values.part_name = selector->Value();
          break;
        case CSSSelector::kPseudoPicker:
          if (selector->Argument() == "select") {
            values.ua_shadow_pseudo = shadow_element_names::kPickerSelect;
          }
          break;
        case CSSSelector::kPseudoIs:
        case CSSSelector::kPseudoWhere:
        case CSSSelector::kPseudoParent: {
          const CSSSelector* selector_list = selector->SelectorListOrParent();
          // If the :is/:where has only a single argument, it effectively acts
          // like a normal selector (save for specificity), and we can put it
          // into a bucket based on that selector.
          //
          // Note that `selector_list` may be nullptr for top-level '&'
          // selectors.
          //
          // Note also that FindBestBucketAndAdd assumes that you cannot
          // reach a pseudo-element via a '&' selector (crbug.com/380107557).
          // We ensure that this cannot happen by never adding rules
          // like '::before { & {} }' to the RuleSet in the first place,
          // see CollectMetadataFromSelector. Rules with mixed
          // allowed/disallowed selectors, e.g. '::before, .foo { & {} }',
          // *are* added to the RuleSet, but fail the IsSingleComplexSelector
          // check below, satisfying the assumptions of FindBestBucketAndAdd.
          if (selector_list &&
              CSSSelectorList::IsSingleComplexSelector(*selector_list)) {
            bool should_continue =
                ExtractBucketingValues(selector_list, style_scope, values);
            CHECK(should_continue);
          }
          break;
        }
        case CSSSelector::kPseudoScope: {
          // Just like :is() and :where(), we can bucket :scope as the
          // <scope-start> it refers to, as long as the <scope-start>
          // contains a single selector.
          //
          // Note that the <scope-start> selector is optional, therefore
          // From() may return nullptr below.
          const CSSSelector* selector_list =
              style_scope ? style_scope->From() : nullptr;
          if (selector_list &&
              CSSSelectorList::IsSingleComplexSelector(*selector_list)) {
            bool should_continue =
                ExtractBucketingValues(selector_list, style_scope, values);
            CHECK(should_continue);
          }
          break;
        }
        default:
          break;
      }
      break;
    case CSSSelector::kAttributeSet:
      values.attr_name = selector->Attribute().LocalName();
      values.attr_value = g_empty_atom;
      break;
    case CSSSelector::kAttributeExact:
    case CSSSelector::kAttributeHyphen:
    case CSSSelector::kAttributeList:
    case CSSSelector::kAttributeContain:
    case CSSSelector::kAttributeBegin:
    case CSSSelector::kAttributeEnd:
      values.is_exact_attr =
          (selector->Match() == CSSSelector::kAttributeExact);
      values.attr_name = selector->Attribute().LocalName();
      values.attr_value = selector->Value();
      break;
    default:
      break;
  }
  return true;
}

// For a (possibly compound) selector, extract the values used for determining
// its buckets (e.g. for “.foo[baz]”, will return foo for class_name and
// baz for attr_name).
static void ExtractBestBucketingValues(const CSSSelector& component,
                                       const StyleScope* style_scope,
                                       BucketingValues& values) {
  for (const CSSSelector* it = &component; it; it = it->NextSimpleSelector()) {
    if (!ExtractBucketingValues(it, style_scope, values)) {
      return;
    }
    switch (it->Relation()) {
      case CSSSelector::kSubSelector:
        continue;
      case CSSSelector::kUAShadow: {
        // Any selector containing ::slotted() currently *must* go in
        // the slotted bucket. Since we allow UA-shadow pseudo-element
        // selectors after ::slotted(), and because such selectors exist
        // in a different compound from ::slotted() (effectively [1]),
        // we have to check if the originating compound contains ::slotted()
        // as well.
        //
        // Note that the same is not true for ::part(); selectors on
        // on the form ::part(p)::ua-shadow must bucket according
        // to ::ua-shadow.
        //
        // This discrepancy comes from the fact that StyleResolver::
        // MatchOuterScopeRules (which handles parts and UA shadow
        // pseudos) does look in the UA shadow bucket across trees,
        // but MatchSlottedRules *only* looks in the slotted bucket.
        // TODO(crbug.com/40068507): This discrepancy is weird.
        //
        // [1] CSSSelectorParser::SplitCompoundAtImplicitCombinator
        const CSSSelector* originating = it->NextSimpleSelector();
        CHECK(originating);
        BucketingValues originating_values;
        ExtractBestBucketingValues(*originating, style_scope,
                                   originating_values);
        values.has_slotted |= originating_values.has_slotted;
        return;
      };
      default:
        // We reached the end of the compound selector.
        return;
    }
  }
}

template <class Func>
static void MarkAsCoveredByBucketing(CSSSelector& selector,
                                     Func&& should_mark_func) {
  for (CSSSelector* s = &selector;;
       UNSAFE_TODO(++s)) {  // Termination condition within loop.
    if (should_mark_func(*s)) {
      s->SetCoveredByBucketing(true);
    }

    // NOTE: We could also have tested single-element :is() and :where()
    // if the inside matches, but it's very rare, so we save the runtime
    // here instead. (& in nesting selectors could perhaps be somewhat
    // more common, but we currently don't bucket on & at all.)
    //
    // We could also have taken universal selectors no matter what
    // should_mark_func() says, but again, we consider that not worth it
    // (though if the selector is being put in the universal bucket,
    // there will be an explicit check).

    if (s->IsLastInComplexSelector() ||
        s->Relation() != CSSSelector::kSubSelector) {
      break;
    }
  }
}

static void UnmarkAsCoveredByBucketing(CSSSelector& selector) {
  for (CSSSelector* s = &selector;;
       UNSAFE_TODO(++s)) {  // Termination condition within loop.
    s->SetCoveredByBucketing(false);
    if (s->IsLastInComplexSelector() ||
        s->Relation() != CSSSelector::kSubSelector) {
      break;
    }
  }
}

template <RuleSet::BucketCoverage bucket_coverage>
void RuleSet::FindBestBucketAndAdd(CSSSelector& component,
                                   const RuleData& rule_data,
                                   const StyleScope* style_scope) {
  BucketingValues values;

#if DCHECK_IS_ON()
  all_rules_.push_back(rule_data);
#endif  // DCHECK_IS_ON()

  ExtractBestBucketingValues(component, style_scope, values);

  // ::slotted() selectors *must* go in the slotted-bucket; we only look
  // for rules in that bucket across shadows.
  if (values.has_slotted) {
    AddToBucket(slotted_pseudo_element_rules_, rule_data);
    return;
  }

  // Similarly, UA-shadow pseudo-element selectors and ::part() selectors
  // must go in their respective buckets, even when there's another selector
  // that is normally considered more specific for bucketing, e.g.
  // ::part(a):hover.

  if (!values.ua_shadow_pseudo.empty()) {
    // Note that `ua_shadow_pseudo` and `part_name` may never be set
    // at the same time due to the implicit combinators [1] inserted before
    // such selectors. This means that it doesn't matter if we try to bucket
    // for `ua_shadow_pseudo` first or for `part_name` first.
    // [1] CSSSelectorParser:: SplitCompoundAtImplicitCombinator.
    CHECK(values.part_name.empty());
    AddToBucket(values.ua_shadow_pseudo, ua_shadow_pseudo_element_rules_,
                rule_data);
    return;
  }

  if (!values.part_name.empty()) {
    CHECK(values.ua_shadow_pseudo.empty()); // See ua_shadow_pseudo branch above.
    // TODO: Mark as covered by bucketing?
    AddToBucket(part_pseudo_rules_, rule_data);
    return;
  }

  // Prefer buckets in order of most likely to apply infrequently.

  if (values.pseudo_type == CSSSelector::kPseudoFocus) {
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
        return selector.Match() == CSSSelector::kPseudoClass &&
               selector.GetPseudoType() == CSSSelector::kPseudoFocus;
      });
    }
    AddToBucket(focus_pseudo_class_rules_, rule_data);
    return;
  }
  if (values.pseudo_type == CSSSelector::kPseudoFocusVisible) {
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
        return selector.Match() == CSSSelector::kPseudoClass &&
               selector.GetPseudoType() == CSSSelector::kPseudoFocusVisible;
      });
    }
    AddToBucket(focus_visible_pseudo_class_rules_, rule_data);
    return;
  }
  if (values.pseudo_type == CSSSelector::kPseudoScrollbarButton ||
      values.pseudo_type == CSSSelector::kPseudoScrollbarCorner ||
      values.pseudo_type == CSSSelector::kPseudoScrollbarThumb ||
      values.pseudo_type == CSSSelector::kPseudoScrollbarTrack ||
      values.pseudo_type == CSSSelector::kPseudoScrollbarTrackPiece) {
    AddToBucket(scrollbar_rules_, rule_data);
    return;
  }
  if (values.pseudo_type == CSSSelector::kPseudoActiveViewTransition) {
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
        return selector.Match() == CSSSelector::kPseudoClass &&
               selector.GetPseudoType() ==
                   CSSSelector::kPseudoActiveViewTransition;
      });
    }
    AddToBucket(active_view_transition_rules_, rule_data);
    return;
  }

  if (!values.id.empty()) {
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(component,
                               [&values](const CSSSelector& selector) {
                                 return selector.Match() == CSSSelector::kId &&
                                        selector.Value() == values.id;
                               });
    }
    AddToBucket(values.id, id_rules_, rule_data);
    return;
  }

  if (!values.class_name.empty()) {
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(
          component, [&values](const CSSSelector& selector) {
            return selector.Match() == CSSSelector::kClass &&
                   selector.Value() == values.class_name;
          });
    }
    AddToBucket(values.class_name, class_rules_, rule_data);
    return;
  }

  if (!values.attr_name.empty()) {
    // input[type="<foo>"] have their own RuleMap.
    if (values.tag_name == html_names::kInputTag.LocalName() &&
        values.attr_name == html_names::kTypeAttr.LocalName() &&
        values.is_exact_attr) {
      // Same logic as tag_name below. Note that this will not
      // mark the rules in the UA stylesheet as covered by bucketing
      // (because they only match elements in the HTML namespace),
      // even though they are the most common input[type="<foo>"] rules.
      if (bucket_coverage == BucketCoverage::kCompute) {
        MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
          return selector.Match() == CSSSelector::kTag &&
                 selector.TagQName().LocalName() ==
                     html_names::kInputTag.LocalName() &&
                 selector.TagQName().NamespaceURI() == g_star_atom;
        });
      }
      AddToBucket(values.attr_value.LowerASCII(), input_rules_, rule_data);
      return;
    }

    AddToBucket(values.attr_name, attr_rules_, rule_data);
    if (values.attr_name == html_names::kStyleAttr) {
      has_bucket_for_style_attr_ = true;
    }
    // NOTE: Cannot mark anything as covered by bucketing, since the bucketing
    // does not verify namespaces. (We could consider doing so if the namespace
    // is *, but we'd need to be careful about case sensitivity wrt. legacy
    // attributes.)
    return;
  }

  if (!values.custom_pseudo_element_name.empty()) {
    // Custom pseudos come before ids and classes in the order of
    // NextSimpleSelector(), and have a relation of ShadowPseudo between them.
    // Therefore we should never be a situation where ExtractSelectorValues
    // finds id and className in addition to custom pseudo.
    DCHECK(values.id.empty());
    DCHECK(values.class_name.empty());
    AddToBucket(values.custom_pseudo_element_name,
                ua_shadow_pseudo_element_rules_, rule_data);
    // TODO: Mark as covered by bucketing?
    return;
  }

  switch (values.pseudo_type) {
    case CSSSelector::kPseudoCue:
      AddToBucket(cue_pseudo_rules_, rule_data);
      return;
    case CSSSelector::kPseudoLink:
    case CSSSelector::kPseudoVisited:
    case CSSSelector::kPseudoAnyLink:
    case CSSSelector::kPseudoWebkitAnyLink:
      if (bucket_coverage == BucketCoverage::kCompute) {
        MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
          // We can only mark kPseudoAnyLink as checked by bucketing;
          // CollectMatchingRules() does not pre-check e.g. whether
          // the link is visible or not.
          return selector.Match() == CSSSelector::kPseudoClass &&
                 (selector.GetPseudoType() == CSSSelector::kPseudoAnyLink ||
                  selector.GetPseudoType() ==
                      CSSSelector::kPseudoWebkitAnyLink);
        });
      }
      AddToBucket(link_pseudo_class_rules_, rule_data);
      return;
    case CSSSelector::kPseudoFocus:
    case CSSSelector::kPseudoFocusVisible:
      // Was handled above.
      NOTREACHED();
      return;
    case CSSSelector::kPseudoSelectorFragmentAnchor:
      AddToBucket(selector_fragment_anchor_rules_, rule_data);
      return;
    case CSSSelector::kPseudoHost:
    case CSSSelector::kPseudoHostContext:
      AddToBucket(shadow_host_rules_, rule_data);
      return;
    case CSSSelector::kPseudoSlotted:
      // Handled above.
      NOTREACHED();
      return;
    case CSSSelector::kPseudoRoot:
      if (bucket_coverage == BucketCoverage::kCompute) {
        MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
          return selector.Match() == CSSSelector::kPseudoClass &&
                 selector.GetPseudoType() == CSSSelector::kPseudoRoot;
        });
      }
      AddToBucket(root_element_rules_, rule_data);
      return;
    default:
      break;
  }

  if (!values.tag_name.empty()) {
    // Covered by bucketing only if the selector would match any namespace
    // (since the bucketing does not take the namespace into account).
    if (bucket_coverage == BucketCoverage::kCompute) {
      MarkAsCoveredByBucketing(
          component, [&values](const CSSSelector& selector) {
            return selector.Match() == CSSSelector::kTag &&
                   selector.TagQName().LocalName() == values.tag_name &&
                   selector.TagQName().NamespaceURI() == g_star_atom;
          });
    }
    AddToBucket(values.tag_name, tag_rules_, rule_data);
    return;
  }

  // The ':scope' pseudo-class (bucketed as universal) may match the host
  // when the selector is scoped (e.g. using '@scope') to that host.
  if (component.IsScopeContaining()) {
    must_check_universal_bucket_for_shadow_host_ = true;
  }

  // Normally, rules involving :host would be stuck in their own bucket
  // above; if we came here, it is because we have something like :is(:host,
  // .foo). Mark that we have this case.
  if (component.IsOrContainsHostPseudoClass()) {
    must_check_universal_bucket_for_shadow_host_ = true;
  }

  // If we didn't find a specialized map to stick it in, file under universal
  // rules.
  MarkAsCoveredByBucketing(component, [](const CSSSelector& selector) {
    return selector.Match() == CSSSelector::kUniversalTag &&
           selector.TagQName() == AnyQName();
  });
  AddToBucket(universal_rules_, rule_data);
}

void RuleSet::AddRule(StyleRule* rule,
                      unsigned selector_index,
                      AddRuleFlags add_rule_flags,
                      const ContainerQuery* container_query,
                      const CascadeLayer* cascade_layer,
                      const StyleScope* style_scope) {
  // The selector index field in RuleData is only 13 bits so we can't support
  // selectors at index 8192 or beyond.
  // See https://crbug.com/804179
  if (selector_index >= (1 << RuleData::kSelectorIndexBits)) {
    return;
  }
  if (rule_count_ >= (1 << RuleData::kPositionBits)) {
    return;
  }
  RuleData rule_data(rule, selector_index, rule_count_, style_scope,
                     add_rule_flags, bloom_hash_backing_);
  ++rule_count_;
  {
    InvalidationSetToSelectorMap::SelectorScope selector_scope(rule,
                                                               selector_index);
    if (features_.CollectFeaturesFromSelector(rule_data.Selector(),
                                              style_scope) ==
        SelectorPreMatch::kNeverMatches) {
      return;
    }
  }

  FindBestBucketAndAdd<BucketCoverage::kCompute>(rule_data.MutableSelector(),
                                                 rule_data, style_scope);

  // If the rule has CSSSelector::kMatchLink, it means that there is a
  // :visited or :link pseudo-class somewhere in the selector. In those cases,
  // we effectively split the rule into two: one which covers the situation
  // where we are in an unvisited link (kMatchLink), and another which covers
  // the visited link case (kMatchVisited).
  if (rule_data.LinkMatchType() == CSSSelector::kMatchLink) {
    // Now the selector will be in two buckets.
    rule_data.ResetEntirelyCoveredByBucketing();

    RuleData visited_dependent(
        rule, rule_data.SelectorIndex(), rule_data.GetPosition(), style_scope,
        add_rule_flags | kRuleIsVisitedDependent, bloom_hash_backing_);
    // Since the selector now is in two buckets, we use
    // BucketCoverage::kIgnore to prevent
    // CSSSelector::is_covered_by_bucketing_ from being set.
    FindBestBucketAndAdd<BucketCoverage::kIgnore>(
        visited_dependent.MutableSelector(), visited_dependent, style_scope);
  }

  AddRuleToLayerIntervals(cascade_layer, rule_data.GetPosition());
  AddRuleToIntervals(container_query, rule_data.GetPosition(),
                     container_query_intervals_);
  AddRuleToIntervals(style_scope, rule_data.GetPosition(), scope_intervals_);
}

void RuleSet::AddRuleToLayerIntervals(const CascadeLayer* cascade_layer,
                                      unsigned position) {
  // nullptr in this context means “no layer”, i.e., the implicit outer layer.
  if (!cascade_layer) {
    if (layer_intervals_.empty()) {
      // Don't create the implicit outer layer if we don't need to.
      return;
    } else {
      cascade_layer = EnsureImplicitOuterLayer();
    }
  }

  AddRuleToIntervals(cascade_layer, position, layer_intervals_);
}

// Similar to AddRuleToLayerIntervals, but for container queries and @style
// scopes.
template <class T>
static void AddRuleToIntervals(const T* value,
                               unsigned position,
                               HeapVector<RuleSet::Interval<T>>& intervals) {
  const T* last_value =
      intervals.empty() ? nullptr : intervals.back().value.Get();
  if (value == last_value) {
    return;
  }

  intervals.push_back(RuleSet::Interval<T>(value, position));
}

void RuleSet::AddPageRule(StyleRulePage* rule) {
  need_compaction_ = true;
  page_rules_.push_back(rule);
}

void RuleSet::AddRouteRule(StyleRuleRoute* rule) {
  need_compaction_ = true;
  route_rules_.push_back(rule);
}

void RuleSet::AddFontFaceRule(StyleRuleFontFace* rule) {
  need_compaction_ = true;
  font_face_rules_.push_back(rule);
}

void RuleSet::AddKeyframesRule(StyleRuleKeyframes* rule) {
  need_compaction_ = true;
  keyframes_rules_.push_back(rule);
}

void RuleSet::AddPropertyRule(StyleRuleProperty* rule) {
  need_compaction_ = true;
  property_rules_.push_back(rule);
}

void RuleSet::AddCounterStyleRule(StyleRuleCounterStyle* rule) {
  need_compaction_ = true;
  counter_style_rules_.push_back(rule);
}

void RuleSet::AddFontPaletteValuesRule(StyleRuleFontPaletteValues* rule) {
  need_compaction_ = true;
  font_palette_values_rules_.push_back(rule);
}

void RuleSet::AddFontFeatureValuesRule(StyleRuleFontFeatureValues* rule) {
  need_compaction_ = true;
  font_feature_values_rules_.push_back(rule);
}

void RuleSet::AddPositionTryRule(StyleRulePositionTry* rule) {
  need_compaction_ = true;
  position_try_rules_.push_back(rule);
}

void RuleSet::AddFunctionRule(StyleRuleFunction* rule) {
  need_compaction_ = true;
  function_rules_.push_back(rule);
}

void RuleSet::AddViewTransitionRule(StyleRuleViewTransition* rule) {
  need_compaction_ = true;
  view_transition_rules_.push_back(rule);
}

void RuleSet::AddChildRules(StyleRule* parent_rule,
                            base::span<const Member<StyleRuleBase>> rules,
                            const MediaQueryEvaluator& medium,
                            const MixinMap& mixins,
                            AddRuleFlags add_rule_flags,
                            const ContainerQuery* container_query,
                            CascadeLayer* cascade_layer,
                            const StyleScope* style_scope,
                            ApplyMixinsStack& apply_mixins_stack) {
  for (StyleRuleBase* rule : rules) {
    if (auto* style_rule = DynamicTo<StyleRule>(rule)) {
      AddStyleRule(style_rule, parent_rule, medium, mixins, add_rule_flags,
                   apply_mixins_stack, container_query, cascade_layer,
                   style_scope);
    } else if (auto* page_rule = DynamicTo<StyleRulePage>(rule)) {
      page_rule->SetCascadeLayer(cascade_layer);
      AddPageRule(page_rule);
    } else if (auto* route_rule = DynamicTo<StyleRuleRoute>(rule)) {
      Document* document = medium.GetMediaValues().GetDocument();
      if (route_rule->GetURLPattern() && document) {
        // A URLPattern becomes an anonymous route. One route for each unique
        // URLPattern.
        RouteMap::Ensure(*document).AddAnonymousRoute(
            route_rule->GetURLPattern());
      }
      if (const auto* route_map = RouteMap::Get(document)) {
        bool matches;
        if (!route_rule->GetName().empty()) {
          matches = route_map->MatchesRoute(route_rule->GetName(),
                                            route_rule->GetPreposition());
        } else {
          DCHECK(route_rule->GetURLPattern());
          matches = route_map->MatchesURLPattern(route_rule->GetURLPattern(),
                                                 route_rule->GetPreposition());
        }
        if (matches) {
          AddChildRules(parent_rule, route_rule->ChildRules(), medium, mixins,
                        add_rule_flags, container_query, cascade_layer,
                        style_scope, apply_mixins_stack);
        }
      }
    } else if (auto* media_rule = DynamicTo<StyleRuleMedia>(rule)) {
      if (MatchMediaForAddRules(medium, media_rule->MediaQueries())) {
        AddChildRules(parent_rule, media_rule->ChildRules(), medium, mixins,
                      add_rule_flags, container_query, cascade_layer,
                      style_scope, apply_mixins_stack);
      }
    } else if (auto* font_face_rule = DynamicTo<StyleRuleFontFace>(rule)) {
      font_face_rule->SetCascadeLayer(cascade_layer);
      AddFontFaceRule(font_face_rule);
    } else if (auto* font_palette_values_rule =
                   DynamicTo<StyleRuleFontPaletteValues>(rule)) {
      // TODO(https://crbug.com/1170794): Handle cascade layers for
      // @font-palette-values.
      AddFontPaletteValuesRule(font_palette_values_rule);
    } else if (auto* font_feature_values_rule =
                   DynamicTo<StyleRuleFontFeatureValues>(rule)) {
      font_feature_values_rule->SetCascadeLayer(cascade_layer);
      AddFontFeatureValuesRule(font_feature_values_rule);
    } else if (auto* keyframes_rule = DynamicTo<StyleRuleKeyframes>(rule)) {
      keyframes_rule->SetCascadeLayer(cascade_layer);
      AddKeyframesRule(keyframes_rule);
    } else if (auto* property_rule = DynamicTo<StyleRuleProperty>(rule)) {
      property_rule->SetCascadeLayer(cascade_layer);
      AddPropertyRule(property_rule);
    } else if (auto* counter_style_rule =
                   DynamicTo<StyleRuleCounterStyle>(rule)) {
      counter_style_rule->SetCascadeLayer(cascade_layer);
      AddCounterStyleRule(counter_style_rule);
    } else if (auto* view_transition_rule =
                   DynamicTo<StyleRuleViewTransition>(rule)) {
      view_transition_rule->SetCascadeLayer(cascade_layer);
      AddViewTransitionRule(view_transition_rule);
    } else if (auto* position_try_rule =
                   DynamicTo<StyleRulePositionTry>(rule)) {
      position_try_rule->SetCascadeLayer(cascade_layer);
      AddPositionTryRule(position_try_rule);
    } else if (auto* function_rule = DynamicTo<StyleRuleFunction>(rule)) {
      function_rule->SetCascadeLayer(cascade_layer);
      AddFunctionRule(function_rule);
    } else if (auto* supports_rule = DynamicTo<StyleRuleSupports>(rule)) {
      if (supports_rule->ConditionIsSupported()) {
        AddChildRules(parent_rule, supports_rule->ChildRules(), medium, mixins,
                      add_rule_flags, container_query, cascade_layer,
                      style_scope, apply_mixins_stack);
      }
    } else if (auto* container_rule = DynamicTo<StyleRuleContainer>(rule)) {
      const ContainerQuery* inner_container_query =
          &container_rule->GetContainerQuery();
      if (container_query) {
        inner_container_query =
            inner_container_query->CopyWithParent(container_query);
      }
      AddChildRules(parent_rule, container_rule->ChildRules(), medium, mixins,
                    add_rule_flags, inner_container_query, cascade_layer,
                    style_scope, apply_mixins_stack);
    } else if (auto* layer_block_rule = DynamicTo<StyleRuleLayerBlock>(rule)) {
      CascadeLayer* sub_layer =
          GetOrAddSubLayer(cascade_layer, layer_block_rule->GetName());
      AddChildRules(parent_rule, layer_block_rule->ChildRules(), medium, mixins,
                    add_rule_flags, container_query, sub_layer, style_scope,
                    apply_mixins_stack);
    } else if (auto* layer_statement_rule =
                   DynamicTo<StyleRuleLayerStatement>(rule)) {
      for (const auto& layer_name : layer_statement_rule->GetNames()) {
        GetOrAddSubLayer(cascade_layer, layer_name);
      }
    } else if (auto* scope_rule = DynamicTo<StyleRuleScope>(rule)) {
      const StyleScope* inner_style_scope = &scope_rule->GetStyleScope();
      if (style_scope) {
        inner_style_scope = inner_style_scope->CopyWithParent(style_scope);
      }
      AddChildRules(parent_rule, scope_rule->ChildRules(), medium, mixins,
                    add_rule_flags, container_query, cascade_layer,
                    inner_style_scope, apply_mixins_stack);
    } else if (auto* starting_style_rule =
                   DynamicTo<StyleRuleStartingStyle>(rule)) {
      AddChildRules(parent_rule, starting_style_rule->ChildRules(), medium,
                    mixins, add_rule_flags | kRuleIsStartingStyle,
                    container_query, cascade_layer, style_scope,
                    apply_mixins_stack);
    } else if (auto* apply_mixin_rule = DynamicTo<StyleRuleApplyMixin>(rule)) {
      ApplyMixin(parent_rule, apply_mixin_rule, medium, mixins, add_rule_flags,
                 container_query, cascade_layer, style_scope,
                 apply_mixins_stack);
    } else if (auto* contents_rule =
                   DynamicTo<StyleRuleContentsStatement>(rule)) {
      const StyleRuleMixin* mixin = apply_mixins_stack.back().mixin;
      const StyleRuleApplyMixin* apply =
          apply_mixins_stack.back().invoking_apply_rule;
      const MixinParameterBindings* mixin_parameter_bindings =
          apply_mixins_stack.back().mixin_parameter_bindings;

      // Verify that the mixin actually has a @contents parameter.
      // Otherwise, @contents is illegal and ignored.
      const bool has_contents_parameter = std::ranges::any_of(
          mixin->GetParameters(),
          [](const StyleRuleFunction::Parameter& parameter) {
            return parameter.name == "@contents";
          });

      if (has_contents_parameter) {
        // Try first the parameter from @apply, then the fallback block given in
        // @contents, and if neither exists, nothing happens.
        StyleRule* rules_to_add = nullptr;
        if (apply->FakeParentRuleForDeclarations()) {
          rules_to_add = apply->FakeParentRuleForDeclarations();
        } else if (contents_rule->FakeParentRuleForFallback() &&
                   contents_rule->FakeParentRuleForFallback()->ChildRules()) {
          rules_to_add = contents_rule->FakeParentRuleForFallback();
        }
        if (rules_to_add && rules_to_add->ChildRules()) {
          rules_to_add = To<StyleRule>(
              rules_to_add->Clone(parent_rule, mixin_parameter_bindings));
          AddChildRules(parent_rule, *rules_to_add->ChildRules(), medium,
                        mixins, add_rule_flags, container_query, cascade_layer,
                        style_scope, apply_mixins_stack);
        }
      }
    } else if (auto* nested_declarations =
                   DynamicTo<StyleRuleNestedDeclarations>(rule)) {
      AddStyleRule(nested_declarations->InnerStyleRule(), parent_rule, medium,
                   mixins, add_rule_flags, apply_mixins_stack, container_query,
                   cascade_layer, style_scope);
    }
  }
}

void RuleSet::ApplyMixin(StyleRule* parent_rule,
                         StyleRuleApplyMixin* apply_mixin_rule,
                         const MediaQueryEvaluator& medium,
                         const MixinMap& mixins,
                         AddRuleFlags add_rule_flags,
                         const ContainerQuery* container_query,
                         CascadeLayer* cascade_layer,
                         const StyleScope* style_scope,
                         ApplyMixinsStack& apply_mixins_stack) {
  auto it = mixins.mixins.find(apply_mixin_rule->GetName());
  if (it != mixins.mixins.end()) {
    StyleRuleMixin* mixin_rule = it->value;
    if (std::ranges::find_if(apply_mixins_stack,
                             [&](const ApplyingMixin& entry) {
                               return entry.mixin == mixin_rule;
                             }) != apply_mixins_stack.end()) {
      // Cycle, so ignore this @apply.
      // NOTE: The exact behavior during cycles is not yet
      // specified. See https://github.com/w3c/csswg-drafts/issues/12595
      return;
    }

    // Bind arguments to parameters.
    if (apply_mixin_rule->GetArguments().size() >
        mixin_rule->GetParameters().size()) {
      // https://drafts.csswg.org/css-mixins/#apply-rule
      // “If passed a <dashed-function>, the arguments passed to the
      // <dashed-function> are mapped to the mixin’s arguments; if more
      // arguments are passed than the length of the mixin’s argument list,
      // the @apply application does nothing. (Passing too few arguments is
      // fine; the missing arguments take their default values instead.)”
      return;
    }

    HashMap<String, std::pair<String, CSSSyntaxDefinition>> bindings;
    for (unsigned i = 0; i < mixin_rule->GetParameters().size(); ++i) {
      const StyleRuleFunction::Parameter& parameter =
          mixin_rule->GetParameters()[i];
      if (i < apply_mixin_rule->GetArguments().size()) {
        bindings.insert(
            parameter.name,
            std::pair(apply_mixin_rule->GetArguments()[i], parameter.type));
      } else if (CSSVariableData* default_value = parameter.default_value;
                 default_value) {
        bindings.insert(parameter.name,
                        std::pair(default_value->OriginalText().ToString(),
                                  parameter.type));
      } else {
        // No parameter given, and no default. This isn't spec-ed yet;
        // see https://github.com/w3c/csswg-drafts/issues/12796.
        // For now, we just don't add a binding (effectively option 2).
      }
    }
    MixinParameterBindings* mixin_parameter_bindings =
        MakeGarbageCollected<MixinParameterBindings>(
            bindings, apply_mixins_stack.empty()
                          ? nullptr
                          : apply_mixins_stack.back().mixin_parameter_bindings);

    apply_mixins_stack.push_back(
        ApplyingMixin{.mixin = mixin_rule,
                      .invoking_apply_rule = apply_mixin_rule,
                      .mixin_parameter_bindings = mixin_parameter_bindings});
    AddChildRules(parent_rule,
                  To<StyleRuleMixin>(
                      mixin_rule->Clone(parent_rule, mixin_parameter_bindings))
                      ->ChildRules(),
                  medium, mixins, add_rule_flags, container_query,
                  cascade_layer, style_scope, apply_mixins_stack);
    apply_mixins_stack.pop_back();

    // If the @mixin we are applying (or currently: any @mixin) was defined
    // inside a media query, we now need to take on the same dependency.
    // This makes sure that if this media query changes, we will also
    // re-evaluate this RuleSet.
    features_.MutableMediaQueryResultFlags().Add(
        mixins.media_query_result_flags);
    media_query_set_results_.AppendVector(mixins.media_query_set_results);

    // Mark that we are using some mixin, and which generation of mixin map
    // it came from, so that we can invalidate if anything should change.
    if (based_on_mixin_generation_ != std::numeric_limits<uint64_t>::max()) {
      CHECK_EQ(based_on_mixin_generation_, mixins.generation);
    }
    based_on_mixin_generation_ = mixins.generation;
  }
}

bool RuleSet::MatchMediaForAddRules(const MediaQueryEvaluator& evaluator,
                                    const MediaQuerySet* media_queries) {
  if (!media_queries) {
    return true;
  }
  bool match_media =
      evaluator.Eval(*media_queries, &features_.MutableMediaQueryResultFlags());
  media_query_set_results_.push_back(
      MediaQuerySetResult(*media_queries, match_media));
  return match_media;
}

void RuleSet::AddRulesFromSheet(const StyleSheetContents* sheet,
                                const MediaQueryEvaluator& medium,
                                const MixinMap& mixins,
                                CascadeLayer* cascade_layer,
                                const StyleScope* style_scope) {
  TRACE_EVENT0("blink", "RuleSet::addRulesFromSheet");
  DCHECK(sheet);

  for (const auto& pre_import_layer : sheet->PreImportLayerStatementRules()) {
    for (const auto& name : pre_import_layer->GetNames()) {
      GetOrAddSubLayer(cascade_layer, name);
    }
  }

  based_on_mixin_generation_ = std::numeric_limits<uint64_t>::max();

  const HeapVector<Member<StyleRuleImport>>& import_rules =
      sheet->ImportRules();
  for (unsigned i = 0; i < import_rules.size(); ++i) {
    StyleRuleImport* import_rule = import_rules[i].Get();
    if (!import_rule->IsSupported()) {
      continue;
    }
    if (!MatchMediaForAddRules(medium, import_rule->MediaQueries())) {
      continue;
    }
    CascadeLayer* import_layer = cascade_layer;
    if (import_rule->IsLayered()) {
      import_layer =
          GetOrAddSubLayer(cascade_layer, import_rule->GetLayerName());
    }
    if (import_rule->GetStyleSheet()) {
      AddRulesFromSheet(import_rule->GetStyleSheet(), medium, mixins,
                        import_layer, import_rule->GetScope());
    }
  }

  InvalidationSetToSelectorMap::StyleSheetContentsScope contents_scope(sheet);
  ApplyMixinsStack apply_mixins_stack;
  AddChildRules(/*parent_rule=*/nullptr, sheet->ChildRules(), medium, mixins,
                kRuleHasNoSpecialState, nullptr /* container_query */,
                cascade_layer, style_scope, apply_mixins_stack);

  if (const auto* route_map = RouteMap::Get(medium.GetDocument())) {
    // Need to do this for every style sheet, since each may add their own
    // anonymous routes.
    //
    // TODO(crbug.com/436805487): See if we can find a better place for this.
    // Maybe RuleSet isn't the right place. DidRoutesChange() was modeled after
    // DidMediaQueryResultsChange(), but maybe there's a better way.
    route_match_state_ = RouteMatchState::Create(*route_map);
  }
}

void RuleSet::NewlyAddedFromDifferentRuleSet(const RuleData& old_rule_data,
                                             const StyleScope* style_scope,
                                             const RuleSet& old_rule_set,
                                             RuleData& new_rule_data) {
  new_rule_data.MovedToDifferentRuleSet(old_rule_set.bloom_hash_backing_,
                                        bloom_hash_backing_, rule_count_);
  // We don't bother with container_query_intervals_ and
  // AddRuleToLayerIntervals() here, since they are not checked in diff
  // rulesets.
  AddRuleToIntervals(style_scope, rule_count_, scope_intervals_);
  ++rule_count_;

#if DCHECK_IS_ON()
  all_rules_.push_back(new_rule_data);
#endif  // DCHECK_IS_ON()
}

void RuleSet::AddFilteredRulesFromOtherBucket(
    const RuleSet& other,
    const HeapVector<RuleData>& src,
    const HeapHashSet<Member<StyleRule>>& only_include,
    HeapVector<RuleData>* dst) {
  Seeker<StyleScope> scope_seeker(other.scope_intervals_);
  for (const RuleData& rule_data : src) {
    if (only_include.Contains(const_cast<StyleRule*>(rule_data.Rule()))) {
      dst->push_back(rule_data);
      NewlyAddedFromDifferentRuleSet(rule_data,
                                     scope_seeker.Seek(rule_data.GetPosition()),
                                     other, dst->back());
    }
  }
}

void RuleSet::AddFilteredRulesFromOtherSet(
    const RuleSet& other,
    const HeapHashSet<Member<StyleRule>>& only_include) {
  if (other.rule_count_ > 0) {
    id_rules_.AddFilteredRulesFromOtherSet(other.id_rules_, only_include, other,
                                           *this);
    class_rules_.AddFilteredRulesFromOtherSet(other.class_rules_, only_include,
                                              other, *this);
    attr_rules_.AddFilteredRulesFromOtherSet(other.attr_rules_, only_include,
                                             other, *this);
    // NOTE: attr_substring_matchers_ will be rebuilt in CompactRules().
    tag_rules_.AddFilteredRulesFromOtherSet(other.tag_rules_, only_include,
                                            other, *this);
    input_rules_.AddFilteredRulesFromOtherSet(other.input_rules_, only_include,
                                              other, *this);
    ua_shadow_pseudo_element_rules_.AddFilteredRulesFromOtherSet(
        other.ua_shadow_pseudo_element_rules_, only_include, other, *this);
    AddFilteredRulesFromOtherBucket(other, other.link_pseudo_class_rules_,
                                    only_include, &link_pseudo_class_rules_);
    AddFilteredRulesFromOtherBucket(other, other.cue_pseudo_rules_,
                                    only_include, &cue_pseudo_rules_);
    AddFilteredRulesFromOtherBucket(other, other.focus_pseudo_class_rules_,
                                    only_include, &focus_pseudo_class_rules_);
    AddFilteredRulesFromOtherBucket(
        other, other.focus_visible_pseudo_class_rules_, only_include,
        &focus_visible_pseudo_class_rules_);
    AddFilteredRulesFromOtherBucket(other, other.scrollbar_rules_, only_include,
                                    &scrollbar_rules_);
    AddFilteredRulesFromOtherBucket(other, other.universal_rules_, only_include,
                                    &universal_rules_);
    AddFilteredRulesFromOtherBucket(other, other.shadow_host_rules_,
                                    only_include, &shadow_host_rules_);
    AddFilteredRulesFromOtherBucket(other, other.part_pseudo_rules_,
                                    only_include, &part_pseudo_rules_);
    AddFilteredRulesFromOtherBucket(other, other.slotted_pseudo_element_rules_,
                                    only_include,
                                    &slotted_pseudo_element_rules_);
    AddFilteredRulesFromOtherBucket(
        other, other.selector_fragment_anchor_rules_, only_include,
        &selector_fragment_anchor_rules_);
    AddFilteredRulesFromOtherBucket(other, other.active_view_transition_rules_,
                                    only_include,
                                    &active_view_transition_rules_);
    AddFilteredRulesFromOtherBucket(other, other.root_element_rules_,
                                    only_include, &root_element_rules_);

    // We don't care about page_rules_ etc., since having those in a RuleSetDiff
    // would mark it as unrepresentable anyway.

    need_compaction_ = true;
  }

#if EXPENSIVE_DCHECKS_ARE_ON()
  allow_unsorted_ = true;
#endif
}

void RuleSet::AddStyleRule(StyleRule* style_rule,
                           StyleRule* parent_rule,
                           const MediaQueryEvaluator& medium,
                           const MixinMap& mixins,
                           AddRuleFlags add_rule_flags,
                           ApplyMixinsStack& apply_mixins_stack,
                           const ContainerQuery* container_query,
                           CascadeLayer* cascade_layer,
                           const StyleScope* style_scope) {
  for (const CSSSelector* selector = style_rule->FirstSelector(); selector;
       selector = CSSSelectorList::Next(*selector)) {
    wtf_size_t selector_index = style_rule->SelectorIndex(*selector);
    AddRule(style_rule, selector_index, add_rule_flags, container_query,
            cascade_layer, style_scope);
  }

  // Nested rules are taken to be added immediately after their parent rule.
  if (style_rule->ChildRules() != nullptr) {
    AddChildRules(style_rule, *style_rule->ChildRules(), medium, mixins,
                  add_rule_flags, container_query, cascade_layer, style_scope,
                  apply_mixins_stack);
  }
}

CascadeLayer* RuleSet::GetOrAddSubLayer(CascadeLayer* cascade_layer,
                                        const StyleRuleBase::LayerName& name) {
  if (!cascade_layer) {
    cascade_layer = EnsureImplicitOuterLayer();
  }
  return cascade_layer->GetOrAddSubLayer(name);
}

bool RuleMap::Add(const AtomicString& key, const RuleData& rule_data) {
  RuleMap::Extent* rules = nullptr;
  if (buckets.IsNull()) {
    // First insert.
    buckets = RobinHoodMap<AtomicString, Extent>(8);
  } else {
    // See if we can find an existing entry for this key.
    RobinHoodMap<AtomicString, Extent>::Bucket* bucket = buckets.Find(key);
    if (bucket != nullptr) {
      rules = &bucket->value;
    }
  }
  if (rules == nullptr) {
    RobinHoodMap<AtomicString, Extent>::Bucket* bucket = buckets.Insert(key);
    if (bucket == nullptr) {
      return false;
    }
    rules = &bucket->value;
    rules->bucket_number = num_buckets++;
  }

  RuleData rule_data_copy = rule_data;
  rule_data_copy.ComputeEntirelyCoveredByBucketing();
  bucket_number_.push_back(rules->bucket_number);
  ++rules->length;
  backing.push_back(std::move(rule_data_copy));
  return true;
}

void RuleMap::Compact() {
  if (compacted) {
    return;
  }
  if (backing.empty()) {
    DCHECK(bucket_number_.empty());
    // Nothing to do.
    compacted = true;
    return;
  }

  backing.shrink_to_fit();

  // Order by (bucket_number, order_in_bucket) by way of a simple
  // in-place counting sort (which is O(n), because our highest bucket
  // number is always less than or equal to the number of elements).
  // First, we make an array that contains the number of elements in each
  // bucket, indexed by the bucket number. We also find each element's
  // position within that bucket.
  auto counts =
      base::HeapArray<unsigned>::WithSize(num_buckets);  // Zero-initialized.
  auto order_in_bucket = base::HeapArray<unsigned>::Uninit(backing.size());
  for (wtf_size_t i = 0; i < bucket_number_.size(); ++i) {
    order_in_bucket[i] = counts[bucket_number_[i]]++;
  }

  // Do the prefix sum. After this, counts[i] is the desired start index
  // for the i-th bucket.
  unsigned sum = 0;
  for (wtf_size_t i = 0; i < num_buckets; ++i) {
    DCHECK_GT(counts[i], 0U);
    unsigned new_sum = sum + counts[i];
    counts[i] = sum;
    sum = new_sum;
  }

  // Store that information into each bucket.
  for (auto& [key, value] : buckets) {
    value.start_index = counts[value.bucket_number];
  }

  // Now put each element into its right place. Every iteration, we will
  // either swap an element into its final destination, or, when we
  // encounter one that is already in its correct place (possibly
  // because we put it there earlier), skip to the next array slot.
  // These will happen exactly n times each, giving us our O(n) runtime.
  for (wtf_size_t i = 0; i < backing.size();) {
    wtf_size_t correct_pos = counts[bucket_number_[i]] + order_in_bucket[i];
    if (i == correct_pos) {
      ++i;
    } else {
      using std::swap;
      swap(backing[i], backing[correct_pos]);
      swap(bucket_number_[i], bucket_number_[correct_pos]);
      swap(order_in_bucket[i], order_in_bucket[correct_pos]);
    }
  }

  // We're done with the bucket numbers, so we can release the memory.
  // If we need the bucket numbers again, they will be reconstructed by
  // RuleMap::Uncompact.
  bucket_number_.clear();

  compacted = true;
}

void RuleMap::Uncompact() {
  bucket_number_.resize(backing.size());

  num_buckets = 0;
  for (auto& [key, value] : buckets) {
    for (unsigned& bucket_number : GetBucketNumberFromExtent(value)) {
      bucket_number = num_buckets;
    }
    value.bucket_number = num_buckets++;
    value.length =
        static_cast<unsigned>(GetBucketNumberFromExtent(value).size());
  }
  compacted = false;
}

// See RuleSet::AddFilteredRulesFromOtherSet().
void RuleMap::AddFilteredRulesFromOtherSet(
    const RuleMap& other,
    const HeapHashSet<Member<StyleRule>>& only_include,
    const RuleSet& old_rule_set,
    RuleSet& new_rule_set) {
  if (compacted) {
    Uncompact();
  }
  if (other.compacted) {
    for (const auto& [key, extent] : other.buckets) {
      Seeker<StyleScope> scope_seeker(old_rule_set.scope_intervals_);
      for (const RuleData& rule_data : other.GetRulesFromExtent(extent)) {
        if (only_include.Contains(const_cast<StyleRule*>(rule_data.Rule()))) {
          Add(key, rule_data);
          new_rule_set.NewlyAddedFromDifferentRuleSet(
              rule_data, scope_seeker.Seek(rule_data.GetPosition()),
              old_rule_set, backing.back());
        }
      }
    }
  } else {
    // First make a mapping of bucket number to key.
    auto keys = base::HeapArray<const AtomicString*>::Uninit(other.num_buckets);
    for (const auto& [key, src_extent] : other.buckets) {
      keys[src_extent.bucket_number] = &key;
    }

    // Now that we have the mapping, we can just copy over all the relevant
    // RuleDatas.
    Seeker<StyleScope> scope_seeker(old_rule_set.scope_intervals_);
    for (wtf_size_t i = 0; i < other.backing.size(); ++i) {
      const unsigned bucket_number = other.bucket_number_[i];
      const RuleData& rule_data = other.backing[i];
      if (only_include.Contains(const_cast<StyleRule*>(rule_data.Rule()))) {
        Add(*keys[bucket_number], rule_data);
        new_rule_set.NewlyAddedFromDifferentRuleSet(
            rule_data, scope_seeker.Seek(rule_data.GetPosition()), old_rule_set,
            backing.back());
      }
    }
  }
}

static wtf_size_t GetMinimumRulesetSizeForSubstringMatcher() {
  // It's not worth going through the Aho-Corasick matcher unless we can
  // reject a reasonable number of rules in one go. Practical ad-hoc testing
  // suggests the break-even point between using the tree and just testing
  // all of the rules individually lies somewhere around 20–40 rules
  // (depending a bit on e.g. how hot the tree is in the cache, the length
  // of the value that we match against, and of course whether we actually
  // have a match). We add a little bit of margin to compensate for the fact
  // that we also need to spend time building the tree, and the extra memory
  // in use.
  return 50;
}

bool RuleSet::CanIgnoreEntireList(base::span<const RuleData> list,
                                  const AtomicString& key,
                                  const AtomicString& value) const {
  DCHECK_EQ(attr_rules_.Find(key).size(), list.size());
  if (!list.empty()) {
    DCHECK_EQ(attr_rules_.Find(key).data(), list.data());
  }
  if (list.size() < GetMinimumRulesetSizeForSubstringMatcher()) {
    // Too small to build up a tree, so always check.
    DCHECK(!base::Contains(attr_substring_matchers_, key));
    return false;
  }

  // See CreateSubstringMatchers().
  if (value.empty()) {
    return false;
  }

  auto it = attr_substring_matchers_.find(key);
  if (it == attr_substring_matchers_.end()) {
    // Building the tree failed, so always check.
    return false;
  }
  return !it->value->AnyMatch(value.LowerASCII().Utf8());
}

void RuleSet::CreateSubstringMatchers(
    RuleMap& attr_map,
    const HeapVector<Interval<StyleScope>>& scope_intervals,
    RuleSet::SubstringMatcherMap& substring_matcher_map) {
  for (const auto& [/*AtomicString*/ attr,
                    /*base::span<const RuleData>*/ ruleset] : attr_map) {
    if (ruleset.size() < GetMinimumRulesetSizeForSubstringMatcher()) {
      continue;
    }
    std::vector<MatcherStringPattern> patterns;
    int rule_index = 0;
    Seeker<StyleScope> scope_seeker(scope_intervals);
    for (const RuleData& rule : ruleset) {
      BucketingValues values;
      const StyleScope* style_scope = scope_seeker.Seek(rule.GetPosition());
      ExtractBestBucketingValues(rule.Selector(), style_scope, values);

      DCHECK(!values.attr_name.empty());

      if (values.attr_value.empty()) {
        if (values.is_exact_attr) {
          // The empty string would make the entire tree useless
          // (it is a substring of every possible value),
          // so as a special case, we ignore it, and have a separate
          // check in CanIgnoreEntireList().
          continue;
        } else {
          // This rule would indeed match every element containing the
          // given attribute (e.g. [foo] or [foo^=""]), so building a tree
          // would be wrong.
          patterns.clear();
          break;
        }
      }

      std::string pattern = values.attr_value.LowerASCII().Utf8();

      // SubstringSetMatcher doesn't like duplicates, and since we only
      // use the tree for true/false information anyway, we can remove them.
      bool already_exists =
          any_of(patterns.begin(), patterns.end(),
                 [&pattern](const MatcherStringPattern& existing_pattern) {
                   return existing_pattern.pattern() == pattern;
                 });
      if (!already_exists) {
        patterns.emplace_back(pattern, rule_index);
      }
      ++rule_index;
    }

    if (patterns.empty()) {
      continue;
    }

    auto substring_matcher = std::make_unique<SubstringSetMatcher>();
    if (!substring_matcher->Build(patterns)) {
      // Should never really happen unless there are megabytes and megabytes
      // of such classes, so we just drop out to the slow path.
    } else {
      substring_matcher_map.insert(attr, std::move(substring_matcher));
    }
  }
}

void RuleSet::CompactRules() {
  DCHECK(need_compaction_);
  id_rules_.Compact();
  class_rules_.Compact();
  attr_rules_.Compact();
  CreateSubstringMatchers(attr_rules_, scope_intervals_,
                          attr_substring_matchers_);
  tag_rules_.Compact();
  input_rules_.Compact();
  ua_shadow_pseudo_element_rules_.Compact();
  link_pseudo_class_rules_.shrink_to_fit();
  cue_pseudo_rules_.shrink_to_fit();
  focus_pseudo_class_rules_.shrink_to_fit();
  selector_fragment_anchor_rules_.shrink_to_fit();
  focus_visible_pseudo_class_rules_.shrink_to_fit();
  scrollbar_rules_.shrink_to_fit();
  universal_rules_.shrink_to_fit();
  shadow_host_rules_.shrink_to_fit();
  part_pseudo_rules_.shrink_to_fit();
  slotted_pseudo_element_rules_.shrink_to_fit();
  active_view_transition_rules_.shrink_to_fit();
  page_rules_.shrink_to_fit();
  font_face_rules_.shrink_to_fit();
  font_palette_values_rules_.shrink_to_fit();
  keyframes_rules_.shrink_to_fit();
  property_rules_.shrink_to_fit();
  counter_style_rules_.shrink_to_fit();
  position_try_rules_.shrink_to_fit();
  layer_intervals_.shrink_to_fit();
  view_transition_rules_.shrink_to_fit();
  bloom_hash_backing_.shrink_to_fit();

#if EXPENSIVE_DCHECKS_ARE_ON()
  if (!allow_unsorted_) {
    AssertRuleListsSorted();
  }
#endif
  need_compaction_ = false;
}

#if EXPENSIVE_DCHECKS_ARE_ON()

namespace {

// Rules that depend on visited link status may be added twice to the same
// bucket (with different LinkMatchTypes).
bool AllowSamePosition(const RuleData& current, const RuleData& previous) {
  return current.LinkMatchType() != previous.LinkMatchType();
}

template <class RuleList>
bool IsRuleListSorted(const RuleList& rules) {
  const RuleData* last_rule = nullptr;
  for (const RuleData& rule : rules) {
    if (last_rule) {
      if (rule.GetPosition() == last_rule->GetPosition()) {
        if (!AllowSamePosition(rule, *last_rule)) {
          return false;
        }
      }
      if (rule.GetPosition() < last_rule->GetPosition()) {
        return false;
      }
    }
    last_rule = &rule;
  }
  return true;
}

}  // namespace

void RuleSet::AssertRuleListsSorted() const {
  for (const auto& item : id_rules_) {
    DCHECK(IsRuleListSorted(item.value));
  }
  for (const auto& item : class_rules_) {
    DCHECK(IsRuleListSorted(item.value));
  }
  for (const auto& item : tag_rules_) {
    DCHECK(IsRuleListSorted(item.value));
  }
  for (const auto& item : input_rules_) {
    DCHECK(IsRuleListSorted(item.value));
  }
  for (const auto& item : ua_shadow_pseudo_element_rules_) {
    DCHECK(IsRuleListSorted(item.value));
  }
  DCHECK(IsRuleListSorted(link_pseudo_class_rules_));
  DCHECK(IsRuleListSorted(cue_pseudo_rules_));
  DCHECK(IsRuleListSorted(focus_pseudo_class_rules_));
  DCHECK(IsRuleListSorted(selector_fragment_anchor_rules_));
  DCHECK(IsRuleListSorted(focus_visible_pseudo_class_rules_));
  DCHECK(IsRuleListSorted(scrollbar_rules_));
  DCHECK(IsRuleListSorted(universal_rules_));
  DCHECK(IsRuleListSorted(shadow_host_rules_));
  DCHECK(IsRuleListSorted(part_pseudo_rules_));
  DCHECK(IsRuleListSorted(active_view_transition_rules_));
}

#endif  // EXPENSIVE_DCHECKS_ARE_ON()

bool RuleSet::DidMediaQueryResultsChange(
    const MediaQueryEvaluator& evaluator) const {
  return evaluator.DidResultsChange(media_query_set_results_);
}

bool RuleSet::DidRoutesChange(const Document* document) const {
  const RouteMap* map = RouteMap::Get(document);
  if (!map || !route_match_state_) {
    return false;
  }
  auto* new_state = RouteMatchState::Create(*map);
  return !new_state->Equals(*route_match_state_);
}

const CascadeLayer* RuleSet::GetLayerForTest(const RuleData& rule) const {
  if (!layer_intervals_.size() ||
      layer_intervals_[0].start_position > rule.GetPosition()) {
    return implicit_outer_layer_.Get();
  }
  for (unsigned i = 1; i < layer_intervals_.size(); ++i) {
    if (layer_intervals_[i].start_position > rule.GetPosition()) {
      return layer_intervals_[i - 1].value.Get();
    }
  }
  return layer_intervals_.back().value.Get();
}

void RuleData::Trace(Visitor* visitor) const {
  visitor->Trace(rule_);
}

template <class T>
void RuleSet::Interval<T>::Trace(Visitor* visitor) const {
  visitor->Trace(value);
}

void RuleSet::Trace(Visitor* visitor) const {
  visitor->Trace(id_rules_);
  visitor->Trace(class_rules_);
  visitor->Trace(attr_rules_);
  visitor->Trace(tag_rules_);
  visitor->Trace(input_rules_);
  visitor->Trace(ua_shadow_pseudo_element_rules_);
  visitor->Trace(link_pseudo_class_rules_);
  visitor->Trace(cue_pseudo_rules_);
  visitor->Trace(focus_pseudo_class_rules_);
  visitor->Trace(selector_fragment_anchor_rules_);
  visitor->Trace(focus_visible_pseudo_class_rules_);
  visitor->Trace(scrollbar_rules_);
  visitor->Trace(universal_rules_);
  visitor->Trace(shadow_host_rules_);
  visitor->Trace(part_pseudo_rules_);
  visitor->Trace(slotted_pseudo_element_rules_);
  visitor->Trace(active_view_transition_rules_);
  visitor->Trace(page_rules_);
  visitor->Trace(font_face_rules_);
  visitor->Trace(font_palette_values_rules_);
  visitor->Trace(font_feature_values_rules_);
  visitor->Trace(view_transition_rules_);
  visitor->Trace(keyframes_rules_);
  visitor->Trace(property_rules_);
  visitor->Trace(route_rules_);
  visitor->Trace(counter_style_rules_);
  visitor->Trace(position_try_rules_);
  visitor->Trace(function_rules_);
  visitor->Trace(root_element_rules_);
  visitor->Trace(media_query_set_results_);
  visitor->Trace(implicit_outer_layer_);
  visitor->Trace(layer_intervals_);
  visitor->Trace(container_query_intervals_);
  visitor->Trace(scope_intervals_);
  visitor->Trace(route_match_state_);
#if DCHECK_IS_ON()
  visitor->Trace(all_rules_);
#endif  // DCHECK_IS_ON()
}

#if DCHECK_IS_ON()
void RuleSet::Show() const {
  for (const RuleData& rule : all_rules_) {
    rule.Selector().Show();
  }
}
#endif  // DCHECK_IS_ON()

}  // namespace blink
