// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

#include "third_party/blink/renderer/core/layout/masonry/masonry_layout_algorithm.h"

#include "base/notreached.h"
#include "third_party/blink/renderer/core/layout/disable_layout_side_effects_scope.h"
#include "third_party/blink/renderer/core/layout/grid/grid_baseline_accumulator.h"
#include "third_party/blink/renderer/core/layout/grid/grid_data.h"
#include "third_party/blink/renderer/core/layout/grid/grid_item.h"
#include "third_party/blink/renderer/core/layout/grid/grid_layout_utils.h"
#include "third_party/blink/renderer/core/layout/grid/grid_track_collection.h"
#include "third_party/blink/renderer/core/layout/grid/grid_track_sizing_algorithm.h"
#include "third_party/blink/renderer/core/layout/layout_utils.h"
#include "third_party/blink/renderer/core/layout/logical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/masonry/layout_masonry.h"
#include "third_party/blink/renderer/core/layout/masonry/masonry_running_positions.h"

namespace blink {

MasonryLayoutAlgorithm::MasonryLayoutAlgorithm(
    const LayoutAlgorithmParams& params)
    : LayoutAlgorithm(params) {
  DCHECK(params.space.IsNewFormattingContext());

  // At various stages of the algorithm we need to know the masonry
  // available-size. If it's initially indefinite, we need to know the min/max
  // sizes as well. Initialize all these to the same value.
  masonry_available_size_ = masonry_min_available_size_ =
      masonry_max_available_size_ = ChildAvailableSize();
  ComputeAvailableSizes(BorderScrollbarPadding(), Node(), GetConstraintSpace(),
                        container_builder_, masonry_available_size_,
                        masonry_min_available_size_,
                        masonry_max_available_size_);

  // TODO(almaher): Apply block-size containment.
}

MinMaxSizesResult MasonryLayoutAlgorithm::ComputeMinMaxSizes(
    const MinMaxSizesFloatInput&) {
  auto ComputeIntrinsicInlineSize = [&](SizingConstraint sizing_constraint) {
    bool needs_intrinsic_track_size = false;
    wtf_size_t start_offset;
    GridItems masonry_items;
    Vector<wtf_size_t> collapsed_track_indexes;
    const ComputedStyle& style = Style();
    const bool is_for_columns =
        style.MasonryTrackSizingDirection() == kForColumns;

    GridSizingTrackCollection track_collection = ComputeGridAxisTracks(
        sizing_constraint, /*intrinsic_repeat_track_sizes=*/nullptr,
        masonry_items, collapsed_track_indexes, start_offset,
        needs_intrinsic_track_size);

    // We have a repeat() track definition with an intrinsic sized track(s). The
    // previous track sizing pass was used to find the track size to apply
    // to the intrinsic sized track(s). Retrieve that value(s), and re-run track
    // sizing to get the correct number of automatic repetitions for the
    // repeat() definition.
    //
    // https://www.w3.org/TR/css-grid-3/#masonry-intrinsic-repeat
    if (needs_intrinsic_track_size) {
      CHECK(collapsed_track_indexes.empty());

      Vector<LayoutUnit> intrinsic_repeat_track_sizes =
          GetIntrinsicRepeaterTrackSizes(!masonry_items.IsEmpty(),
                                         track_collection);
      track_collection = ComputeGridAxisTracks(
          sizing_constraint, &intrinsic_repeat_track_sizes, masonry_items,
          collapsed_track_indexes, start_offset, needs_intrinsic_track_size);
    }

    if (is_for_columns) {
      // Track sizing is done during the guess placement step, which happens in
      // `BuildGridAxisTracks`, so at this point, getting the width of all of
      // the columns should correctly give us the intrinsic inline size.
      return track_collection.CalculateSetSpanSize();
    } else {
      if (masonry_items.IsEmpty()) {
        // If there are no masonry items, the intrinsic inline size is only
        // border, scrollbar, and padding.
        return BorderScrollbarPadding().InlineSum();
      }

      MasonryRunningPositions running_positions(
          track_collection, style,
          ResolveItemToleranceForMasonry(style, masonry_available_size_),
          collapsed_track_indexes);

      PlaceMasonryItems(track_collection, masonry_items, start_offset,
                        running_positions, sizing_constraint);
      // `stacking_axis_gap` represents the space between each of the items
      // in the row. We need to subtract this as it is always added to
      // `running_positions` whenever an item is placed, but the very last
      // addition should be deleted as there is no item after it.
      const auto stacking_axis_gap =
          GridTrackSizingAlgorithm::CalculateGutterSize(
              style, masonry_available_size_, kForColumns);
      return running_positions.GetMaxPositionForSpan(
                 GridSpan::TranslatedDefiniteGridSpan(
                     /*start_line=*/0,
                     /*end_line=*/track_collection.EndLineOfImplicitGrid())) -
             stacking_axis_gap;
    }
  };

  MinMaxSizes intrinsic_sizes{
      ComputeIntrinsicInlineSize(SizingConstraint::kMinContent),
      ComputeIntrinsicInlineSize(SizingConstraint::kMaxContent)};
  intrinsic_sizes += BorderScrollbarPadding().InlineSum();

  // TODO(ethavar): Compute `depends_on_block_constraints` by checking if any
  // masonry item has `is_sizing_dependent_on_block_size` set to true.
  return {intrinsic_sizes, /*depends_on_block_constraints=*/false};
}

const LayoutResult* MasonryLayoutAlgorithm::Layout() {
  bool needs_intrinsic_track_size = false;
  wtf_size_t start_offset;
  GridItems masonry_items;
  HeapVector<Member<LayoutBox>> oof_children;
  Vector<wtf_size_t> collapsed_track_indexes;

  GridSizingTrackCollection track_collection = ComputeGridAxisTracks(
      SizingConstraint::kLayout, /*intrinsic_repeat_track_sizes=*/nullptr,
      masonry_items, collapsed_track_indexes, start_offset,
      needs_intrinsic_track_size, &oof_children);

  // We have a repeat() track definition with an intrinsic sized track(s). The
  // previous track sizing pass was used to find the track size to apply
  // to the intrinsic sized track(s). Retrieve that value(s), and re-run track
  // sizing to get the correct number of automatic repetitions for the
  // repeat() definition.
  //
  // https://www.w3.org/TR/css-grid-3/#masonry-intrinsic-repeat
  if (needs_intrinsic_track_size) {
    CHECK(collapsed_track_indexes.empty());

    Vector<LayoutUnit> intrinsic_repeat_track_sizes =
        GetIntrinsicRepeaterTrackSizes(!masonry_items.IsEmpty(),
                                       track_collection);
    track_collection = ComputeGridAxisTracks(
        SizingConstraint::kLayout, &intrinsic_repeat_track_sizes, masonry_items,
        collapsed_track_indexes, start_offset, needs_intrinsic_track_size);
  }

  if (!masonry_items.IsEmpty()) {
    MasonryRunningPositions running_positions(
        track_collection, Style(),
        ResolveItemToleranceForMasonry(Style(), masonry_available_size_),
        collapsed_track_indexes);

    PlaceMasonryItems(track_collection, masonry_items, start_offset,
                      running_positions, SizingConstraint::kLayout);
  }

  // Create track layout data to support masonry overlay in DevTools.
  std::unique_ptr<GridLayoutData> layout_data(
      std::make_unique<GridLayoutData>());
  layout_data->SetTrackCollection(
      std::make_unique<GridLayoutTrackCollection>(track_collection));

  // Account for border, scrollbar, and padding in the intrinsic block size.
  intrinsic_block_size_ += BorderScrollbarPadding().BlockSum();
  const auto block_size = ComputeBlockSizeForFragment(
      GetConstraintSpace(), Node(), BorderPadding(), intrinsic_block_size_,
      container_builder_.InlineSize());
  container_builder_.SetFragmentsTotalBlockSize(block_size);
  container_builder_.SetIntrinsicBlockSize(intrinsic_block_size_);

  // Place out-of-flow items after setting the intrinsic block size, since
  // out-of-flow items don't contribute to the intrinsic size of the container.
  if (!oof_children.empty()) {
    PlaceOutOfFlowItems(*layout_data, block_size, oof_children);
  }

  container_builder_.TransferGridLayoutData(std::move(layout_data));
  container_builder_.HandleOofsAndSpecialDescendants();
  return container_builder_.ToBoxFragment();
}

namespace {

// TODO(almaher): Should we consolidate this with LayoutGridItemForMeasure()?
const LayoutResult* LayoutMasonryItemForMeasure(
    const GridItemData& masonry_item,
    const ConstraintSpace& constraint_space,
    SizingConstraint sizing_constraint) {
  const auto& node = masonry_item.node;

  // Disable side effects during MinMax computation to avoid potential "MinMax
  // after layout" crashes. This is not necessary during the layout pass, and
  // would have a negative impact on performance if used there.
  //
  // TODO(ikilpatrick): For subgrid, ideally we don't want to disable side
  // effects as it may impact performance significantly; this issue can be
  // avoided by introducing additional cache slots (see crbug.com/1272533).
  //
  // TODO(almaher): Handle submasonry here.
  std::optional<DisableLayoutSideEffectsScope> disable_side_effects;
  if (!node.GetLayoutBox()->NeedsLayout() &&
      sizing_constraint != SizingConstraint::kLayout) {
    disable_side_effects.emplace();
  }
  return node.Layout(constraint_space);
}

LayoutUnit AlignContentOffset(
    LayoutUnit intrinsic_size,
    LayoutUnit container_size,
    LayoutUnit baseline_offset,
    const StyleContentAlignmentData& content_alignment) {
  // Note: There is only ever one alignment subject for these properties in the
  // stacking axis, so the unique align-content / justify-content values boil
  // down to start, center, end, and baseline alignment. (The behavior of normal
  // and stretch is identical to start, and the distributed alignment values
  // behave as their fallback alignments.) [1].
  //
  // [1]: https://www.w3.org/TR/css-grid-3/#alignment
  LayoutUnit free_space = container_size - intrinsic_size;

  // If overflow is 'safe', we have to make sure we don't overflow the
  // 'start' edge (potentially cause some data loss as the overflow is
  // unreachable).
  if (content_alignment.Overflow() == OverflowAlignment::kSafe) {
    free_space = free_space.ClampNegativeToZero();
  }

  switch (content_alignment.Distribution()) {
    case ContentDistributionType::kSpaceAround:
    case ContentDistributionType::kSpaceEvenly:
      return (free_space / 2);
    case ContentDistributionType::kSpaceBetween:
    case ContentDistributionType::kStretch:
    case ContentDistributionType::kDefault:
      break;
  }

  switch (content_alignment.GetPosition()) {
    case ContentPosition::kLeft:
    case ContentPosition::kStart:
    case ContentPosition::kFlexStart:
    case ContentPosition::kNormal:
      return LayoutUnit();
    case ContentPosition::kCenter:
      return (free_space / 2);
    case ContentPosition::kRight:
    case ContentPosition::kEnd:
    case ContentPosition::kFlexEnd:
      return free_space;
    case ContentPosition::kBaseline:
    case ContentPosition::kLastBaseline:
      return baseline_offset;
  }
  NOTREACHED();
}

}  // namespace

LayoutUnit MasonryLayoutAlgorithm::CalculateItemInlineContribution(
    const GridItemData& masonry_item,
    SizingConstraint sizing_constraint) {
  CHECK_NE(sizing_constraint, SizingConstraint::kLayout);
  // We need to compute the available space for the item if we are using it
  // to compute min/max content sizes.
  const ConstraintSpace space_for_measure =
      CreateConstraintSpaceForMeasure(masonry_item);
  const MinMaxSizes sizes = ComputeMinAndMaxContentContributionForSelf(
                                masonry_item.node, space_for_measure)
                                .sizes;
  return (sizing_constraint == SizingConstraint::kMinContent) ? sizes.min_size
                                                              : sizes.max_size;
}

// TODO(almaher): Item margins aren't being taken into account for placement.
void MasonryLayoutAlgorithm::PlaceMasonryItems(
    const GridLayoutTrackCollection& track_collection,
    GridItems& masonry_items,
    wtf_size_t start_offset,
    MasonryRunningPositions& running_positions,
    std::optional<SizingConstraint> sizing_constraint) {
  const auto& border_scrollbar_padding = BorderScrollbarPadding();
  const auto& container_space = GetConstraintSpace();
  const auto& style = Style();
  const bool is_for_layout = sizing_constraint == SizingConstraint::kLayout;

  const auto container_writing_direction =
      container_space.GetWritingDirection();
  const auto grid_axis_direction = track_collection.Direction();
  const bool is_for_columns = grid_axis_direction == kForColumns;
  const auto stacking_axis_gap = GridTrackSizingAlgorithm::CalculateGutterSize(
      style, masonry_available_size_, is_for_columns ? kForRows : kForColumns);

  // TODO(kschmi): Handle baselines in the stacking direction, depending on the
  // resolution for https://github.com/w3c/csswg-drafts/issues/9530.
  GridBaselineAccumulator baseline_accumulator(style.GetFontBaseline());

  for (auto& masonry_item : masonry_items) {
    // Get the starting offset of where we want the item placed in the stacking
    // axis.
    LayoutUnit start_offset_in_stacking_axis =
        running_positions.FinalizeItemSpanAndGetMaxPosition(
            start_offset, masonry_item, track_collection);

    // During track sizing, we may force a specific inline size on an item
    // if the available space in that direction is indefinite, particularly for
    // orthogonal items. In Grid, that constraint is maintained during layout
    // due to the two dimensional nature of Grid tracks. In Masonry, recompute
    // this fixed size to guarantee we maintain the same constraint during track
    // sizing and layout.
    std::optional<LayoutUnit> opt_fixed_inline_size;
    if (is_for_layout) {
      const ConstraintSpace space_for_measure =
          CreateConstraintSpaceForMeasure(masonry_item);
      if (space_for_measure.AvailableSize().inline_size == kIndefiniteSize) {
        const MinMaxSizes sizes = ComputeMinAndMaxContentContributionForSelf(
                                      masonry_item.node, space_for_measure)
                                      .sizes;
        opt_fixed_inline_size = sizes.max_size;
      }
    }

    // TODO(celestepan): Rename `containing_rect` to `item_rect` or something
    // that better represents the fact that it only contains the current masonry
    // item we are working with.
    //
    // This item is ultimately placed below the maximum running position among
    // its spanned tracks. Account for border, scrollbar, and padding in the
    // offset of the item.
    LogicalRect containing_rect;

    const ConstraintSpace space =
        is_for_layout ? CreateConstraintSpaceForLayout(
                            masonry_item, track_collection,
                            opt_fixed_inline_size, &containing_rect)
                      : CreateConstraintSpaceForMeasure(
                            masonry_item,
                            CalculateItemInlineContribution(masonry_item,
                                                            *sizing_constraint),
                            /*is_for_min_max_sizing=*/true);

    const auto& item_node = masonry_item.node;
    const auto& item_style = item_node.Style();
    const LayoutResult* result =
        is_for_layout ? result = item_node.Layout(space)
                      : LayoutMasonryItemForMeasure(masonry_item, space,
                                                    *sizing_constraint);

    const auto& physical_fragment =
        To<PhysicalBoxFragment>(result->GetPhysicalFragment());
    const LogicalBoxFragment fragment(container_writing_direction,
                                      physical_fragment);
    const auto margins = ComputeMarginsFor(space, item_style, container_space);
    const LayoutUnit fragment_size =
        is_for_columns ? fragment.BlockSize() + margins.BlockSum()
                       : fragment.InlineSize() + margins.InlineSum();

    // If dense packing is set, we need to figure out if the item can possibly
    // fit into any previous track openings. If it can, then we need to adjust
    // `item_span` as well as the offset of `containing_rect`, which is sized
    // based on the items within the masonry container. Margins need to be added
    // to the item's size in the stacking axis.
    const bool is_dense_packing = style.IsGridAutoFlowAlgorithmDense();
    bool item_moved_to_earlier_opening = false;
    if (is_dense_packing) {
      LayoutUnit updated_item_start_offset =
          running_positions.GetEligibleTrackOpeningAndUpdateMasonryItemSpan(
              start_offset, masonry_item,
              /*item_stacking_axis_contribution=*/fragment_size +
                  stacking_axis_gap,
              track_collection);

      // If we have a valid offset for the item in the stacking axis, it means
      // we found an earlier track opening for the item.
      if (updated_item_start_offset != LayoutUnit::Max()) {
        // Because it's possible that we switched the item to a different span,
        // update the offset of where the item should be placed in the grid
        // axis.
        const LayoutUnit masonry_item_start_offset =
            track_collection.GetSetOffset(
                masonry_item.SetIndices(track_collection.Direction()).begin);
        is_for_columns
            ? containing_rect.offset.inline_offset = masonry_item_start_offset
            : containing_rect.offset.block_offset = masonry_item_start_offset;

        item_moved_to_earlier_opening = true;
        start_offset_in_stacking_axis = updated_item_start_offset;
      }
    }

    // Update auto-placement cursor after we have determined the item's final
    // placement.
    running_positions.UpdateAutoPlacementCursor(
        masonry_item.resolved_position.EndLine(grid_axis_direction));

    // `start_offset_in_stacking_axis` specifies where in the stacking axis the
    // item should be placed, so we need to adjust the `containing_rect` in the
    // stacking axis to accommodate the newly placed item.
    is_for_columns ? containing_rect.offset.block_offset =
                         start_offset_in_stacking_axis +
                         border_scrollbar_padding.block_start
                   : containing_rect.offset.inline_offset =
                         start_offset_in_stacking_axis +
                         border_scrollbar_padding.inline_start;

    // TODO(celestepan): Account for extra margins from sub-masonry items.
    //
    // Adjust item's position in the track based on style. We only want offset
    // applied to the grid axis at the moment.
    //
    // TODO(celestepan): Update alignment logic if needed once we resolve on
    // https://github.com/w3c/csswg-drafts/issues/10275.

    const auto inline_alignment =
        is_for_columns ? masonry_item.Alignment(kForColumns) : AxisEdge::kStart;
    const auto block_alignment =
        is_for_columns ? AxisEdge::kStart : masonry_item.Alignment(kForRows);
    containing_rect.offset += LogicalOffset(
        AlignmentOffset(containing_rect.size.inline_size, fragment.InlineSize(),
                        margins.inline_start, margins.inline_end,
                        /*baseline_offset=*/LayoutUnit(), inline_alignment,
                        masonry_item.IsOverflowSafe(kForColumns)),
        AlignmentOffset(containing_rect.size.block_size, fragment.BlockSize(),
                        margins.block_start, margins.block_end,
                        /*baseline_offset=*/LayoutUnit(), block_alignment,
                        masonry_item.IsOverflowSafe(kForRows)));

    // If the item was not placed in an earlier track opening, update
    // `running_positions` of the tracks that the items spans to include the
    // size of the item, the size of the opening in the stacking axis, and the
    // margin.
    if (!item_moved_to_earlier_opening) {
      auto new_running_position =
          start_offset_in_stacking_axis + stacking_axis_gap + fragment_size;

      // If dense packing is enabled, we need to input the maximum running
      // position of the tracks our items span so that we can account for any
      // new openings that may form.
      running_positions.UpdateRunningPositionsForSpan(
          masonry_item.resolved_position.Span(grid_axis_direction),
          new_running_position,
          is_dense_packing
              ? std::make_optional(
                    /*max_running_position=*/start_offset_in_stacking_axis)
              : std::nullopt);
    }

    container_builder_.AddResult(*result, containing_rect.offset, margins);
    baseline_accumulator.Accumulate(masonry_item, fragment,
                                    containing_rect.offset.block_offset);
  }

  // Propagate the baselines to the container.
  if (auto first_baseline = baseline_accumulator.FirstBaseline()) {
    container_builder_.SetFirstBaseline(*first_baseline);
  }
  if (auto last_baseline = baseline_accumulator.LastBaseline()) {
    container_builder_.SetLastBaseline(*last_baseline);
  }

  // Determine intrinsic size of the masonry container. For the stacking axis,
  // remove the last gap that was added, since there is no item after it.
  const LayoutUnit stacking_axis_size =
      running_positions.GetMaxPositionForSpan(
          GridSpan::TranslatedDefiniteGridSpan(
              /*start_line=*/0,
              /*end_line=*/track_collection.EndLineOfImplicitGrid())) -
      stacking_axis_gap;

  // To determine the size of the grid axis, add the size of the tracks.
  const LayoutUnit grid_axis_size = track_collection.CalculateSetSpanSize();
  intrinsic_block_size_ = is_for_columns ? stacking_axis_size : grid_axis_size;

  // Apply content alignment/justification. This is an additional offset
  // determined by the intrisic inline or block size of the masonry container,
  // so it must occur after that has been determined. This must also occur after
  // the container baselines have been set.
  const auto& content_alignment =
      is_for_columns ? style.AlignContent() : style.JustifyContent();
  if (content_alignment != ComputedStyleInitialValues::InitialAlignContent()) {
    const LayoutUnit intrinsic_inline_size =
        is_for_columns ? grid_axis_size : stacking_axis_size;

    const LayoutUnit align_content_offset = AlignContentOffset(
        is_for_columns ? intrinsic_block_size_ : intrinsic_inline_size,
        is_for_columns ? ChildAvailableSize().block_size
                       : ChildAvailableSize().inline_size,
        baseline_accumulator.FirstBaseline().value_or(LayoutUnit()),
        content_alignment);

    if (is_for_columns) {
      if (ChildAvailableSize().block_size != kIndefiniteSize) {
        container_builder_.MoveChildrenInDirection(align_content_offset,
                                                   /*is_block_direction=*/true);
      }
    } else {
      if (ChildAvailableSize().inline_size != kIndefiniteSize) {
        container_builder_.MoveChildrenInDirection(
            align_content_offset, /*is_block_direction=*/false);
      }
    }
  }
}

void MasonryLayoutAlgorithm::PlaceOutOfFlowItems(
    const GridLayoutData& layout_data,
    LayoutUnit block_size,
    HeapVector<Member<LayoutBox>>& oof_children) {
  const auto& container_style = Style();
  const auto& node = Node();
  const auto& placement_data = node.CachedPlacementData();
  const LogicalSize total_fragment_size = {container_builder_.InlineSize(),
                                           block_size};
  const auto default_containing_block_size =
      ShrinkLogicalSize(total_fragment_size, BorderScrollbarPadding());

  for (LayoutBox* oof_child : oof_children) {
    GridItemData* out_of_flow_item = MakeGarbageCollected<GridItemData>(
        BlockNode(oof_child), container_style);
    DCHECK(out_of_flow_item->IsOutOfFlow());

    std::optional<LogicalRect> containing_block_rect;
    const auto position = out_of_flow_item->node.Style().GetPosition();

    // If the masonry container is also the containing-block for the
    // OOF-positioned item, pick up the static-position from the grid-area
    // in the grid axis.
    if ((node.IsAbsoluteContainer() && position == EPosition::kAbsolute) ||
        (node.IsFixedContainer() && position == EPosition::kFixed)) {
      containing_block_rect.emplace(ComputeOutOfFlowItemContainingRect(
          placement_data, layout_data, container_style,
          container_builder_.Borders(), total_fragment_size,
          BorderScrollbarPadding(), out_of_flow_item));
    }

    auto child_offset = containing_block_rect
                            ? containing_block_rect->offset
                            : BorderScrollbarPadding().StartOffset();
    const auto containing_block_size = containing_block_rect
                                           ? containing_block_rect->size
                                           : default_containing_block_size;

    LogicalStaticPosition::InlineEdge inline_edge;
    LogicalStaticPosition::BlockEdge block_edge;

    AlignmentOffsetForOutOfFlow(out_of_flow_item->Alignment(kForColumns),
                                out_of_flow_item->Alignment(kForRows),
                                containing_block_size, &inline_edge,
                                &block_edge, &child_offset);

    // TODO(kschmi): Handle fragmentation.
    container_builder_.AddOutOfFlowChildCandidate(
        out_of_flow_item->node, child_offset, inline_edge, block_edge);
  }
}

GridItems MasonryLayoutAlgorithm::BuildVirtualMasonryItems(
    const GridLineResolver& line_resolver,
    const GridItems& masonry_items,
    const bool needs_intrinsic_track_size,
    SizingConstraint sizing_constraint,
    const wtf_size_t auto_repetition_count,
    wtf_size_t& start_offset) const {
  const auto& style = Style();
  const auto grid_axis_direction = style.MasonryTrackSizingDirection();
  const bool is_for_columns = grid_axis_direction == kForColumns;

  const LayoutUnit grid_axis_gap =
      GridTrackSizingAlgorithm::CalculateGutterSize(
          style, masonry_available_size_,
          is_for_columns ? kForColumns : kForRows);

  wtf_size_t max_end_line;
  GridItems virtual_items;

  // If there is an auto-fit track definition, store what tracks it spans.
  const GridTrackList& track_list =
      is_for_columns ? style.GridTemplateColumns().GetTrackList()
                     : style.GridTemplateRows().GetTrackList();
  GridSpan auto_fit_span = GridSpan::IndefiniteGridSpan();
  if (!needs_intrinsic_track_size && track_list.HasAutoRepeater() &&
      track_list.RepeatType(track_list.AutoRepeatTrackIndex()) ==
          GridTrackRepeater::RepeatType::kAutoFit) {
    auto_fit_span = GridSpan::TranslatedDefiniteGridSpan(
        track_list.TrackCountBeforeAutoRepeat(),
        track_list.TrackCountBeforeAutoRepeat() + auto_repetition_count);
  }

  wtf_size_t unplaced_item_span_count = 0;

  for (const auto& [group_items, group_properties] :
       Node().CollectItemGroups(line_resolver, masonry_items, max_end_line,
                                start_offset, unplaced_item_span_count)) {
    auto* virtual_item = MakeGarbageCollected<GridItemData>();

    GridSpan span = group_properties.Span();
    wtf_size_t span_size = span.SpanSize();
    CHECK_GT(span_size, 0u);

    for (const Member<GridItemData>& group_item : group_items) {
      const GridItemData& item_data = *group_item;
      const BlockNode& item_node = item_data.node;
      const auto space = CreateConstraintSpaceForMeasure(item_data);
      const ComputedStyle& item_style = item_node.Style();

      const bool is_parallel_with_track_direction =
          is_for_columns == item_data.is_parallel_with_root_grid;

      // TODO(almaher): Subgrids have extra margin to handle unique gap sizes.
      // This requires access to the subgrid track collection, where that extra
      // margin is accumulated.
      const BoxStrut margins =
          ComputeMarginsFor(space, item_style, GetConstraintSpace());
      const LayoutUnit margin_sum =
          is_for_columns ? margins.InlineSum() : margins.BlockSum();

      MinMaxSizes min_max_contribution;
      if (is_parallel_with_track_direction) {
        min_max_contribution =
            ComputeMinAndMaxContentContributionForSelf(item_node, space).sizes;
      } else {
        LayoutUnit block_contribution = ComputeMasonryItemBlockContribution(
            grid_axis_direction, sizing_constraint, space, &item_data,
            needs_intrinsic_track_size);
        min_max_contribution =
            MinMaxSizes(block_contribution, block_contribution);
      }

      // Keep track of special item contributions for intrinsic minimums. This
      // logic can depend on the tracks the item spans, so store three different
      // contributions - one assuming that the items are spanning such tracks,
      // and two assuming they aren't (one that may need to be clamped and one
      // that doesn't), so that later we can choose one or the other depending
      // on the tracks the virtual item spans. If a contribution may need to be
      // clamped, `maybe_clamp` will be set to true. See
      // https://drafts.csswg.org/css-grid/#min-size-auto for more details.
      //
      // TODO(almaher): Pass in the correct baseline shim.
      //
      // TODO(almaher): pass in `subgrid_minmax_sizes` when we support
      // subgrid.
      bool maybe_clamp = false;
      LayoutUnit contribution_assuming_tracks =
          CalculateIntrinsicMinimumContribution(
              is_parallel_with_track_direction,
              /*special_spanning_criteria=*/true, min_max_contribution.min_size,
              min_max_contribution.max_size, space,
              /*subgrid_minmax_sizes=*/MinMaxSizesResult(), &item_data,
              maybe_clamp);
      // If we assume we are spanning tracks that force us to use the automatic
      // min size, we will never need to clamp the value returned here. As such,
      // `maybe_clamp` should never be true if `special_spanning_criteria` is
      // true.
      CHECK(!maybe_clamp);

      // It is ok to use the same `maybe_clamp` var here since the previous call
      // will never produce clamping, and the next call is the one we care about
      // potentially clamping.
      LayoutUnit contribution_ignoring_tracks =
          CalculateIntrinsicMinimumContribution(
              is_parallel_with_track_direction,
              /*special_spanning_criteria=*/false,
              min_max_contribution.min_size, min_max_contribution.max_size,
              space, /*subgrid_minmax_sizes=*/MinMaxSizesResult(), &item_data,
              maybe_clamp);

      // Add the margin sum to all contribution sizes, and adjust each
      // if we are running an initial track sizing pass for intrinsic
      // auto repeats.
      const LayoutUnit total_gap_spanned = grid_axis_gap * (span_size - 1);
      auto AdjustItemContribution = [&](LayoutUnit& contribution_size) {
        contribution_size += margin_sum;

        // We have a repeat() track definition with an intrinsic sized track(s).
        // The current track sizing pass is used to find the track size to apply
        // to the intrinsic sized track(s). If the current item spans more than
        // one track, treat it as if it spans one track per the intrinsic
        // tracks and repeat algorithm [1].
        //
        // [1] https://www.w3.org/TR/css-grid-3/#masonry-intrinsic-repeat
        if (needs_intrinsic_track_size && span_size > 1) {
          contribution_size -= total_gap_spanned;
          contribution_size /= LayoutUnit(span_size);
        }
      };
      AdjustItemContribution(min_max_contribution.min_size);
      AdjustItemContribution(min_max_contribution.max_size);
      AdjustItemContribution(contribution_ignoring_tracks);
      AdjustItemContribution(contribution_assuming_tracks);

      // Store the different contribution sizes on the virtual item to be used
      // later during track sizing.
      virtual_item->EncompassContributionSize(min_max_contribution);
      virtual_item->EncompassIntrinsicMinAssumingTrackPlacement(
          contribution_assuming_tracks);
      if (maybe_clamp) {
        virtual_item->EncompassIntrinsicMinIgnoringTrackPlacement(
            contribution_ignoring_tracks);

        const auto border_padding = ComputeBorders(space, item_node) +
                                    ComputePadding(space, item_style);
        const auto border_padding_sum = is_parallel_with_track_direction
                                            ? border_padding.InlineSum()
                                            : border_padding.BlockSum();

        // TODO(almaher): The min clamp size should include baseline shim.
        virtual_item->EncompassMinClampSize(margin_sum + border_padding_sum);
      } else {
        virtual_item->EncompassIntrinsicMinIgnoringTrackPlacementUnclamped(
            contribution_ignoring_tracks);
      }
    }

    // If `needs_intrinsic_track_size` is true, that means we have a repeat()
    // track definition with an intrinsic sized track(s). The current track
    // sizing pass is used to find the track size to apply to the intrinsic
    // sized track(s). Ignore item placement as part of this pass, and apply all
    // items in every position, regardless of explicit placement [1].
    //
    // [1] https://www.w3.org/TR/css-grid-3/#masonry-intrinsic-repeat
    if (span.IsIndefinite() || needs_intrinsic_track_size) {
      // For groups of items that are auto-placed, we need to create copies of
      // the virtual item and place them at each possible start line. At the end
      // of the loop below, `span` will be located at the last start line, which
      // should be the position of the last copy appended to `virtual_items`.
      if (needs_intrinsic_track_size) {
        span = GridSpan::TranslatedDefiniteGridSpan(0, 1);
      } else {
        span =
            GridSpan::TranslatedDefiniteGridSpan(0, span.IndefiniteSpanSize());
      }

      while (span.EndLine() < max_end_line) {
        auto* item_copy = MakeGarbageCollected<GridItemData>(*virtual_item);
        item_copy->resolved_position.SetSpan(span, grid_axis_direction);
        virtual_items.Append(std::move(item_copy));

        // `Translate` will move the span to the start and end of the next line,
        // allowing us to "slide" over the entire implicit grid.
        span.Translate(1);

        // Per the auto-fit heuristic, don't add auto placed items to tracks
        // within the auto-fit range that are greater than the total span count
        // of auto placed items.
        //
        // https://drafts.csswg.org/css-grid-3/#repeat-auto-fit
        if (!auto_fit_span.IsIndefinite()) {
          while (span.Intersects(auto_fit_span) &&
                 span.EndLine() > unplaced_item_span_count) {
            span.Translate(1);
          }
        }
      }
    }

    DCHECK(span.IsTranslatedDefinite());
    if (span.EndLine() <= max_end_line) {
      virtual_item->resolved_position.SetSpan(span, grid_axis_direction);
      virtual_items.Append(virtual_item);
    }
  }
  return virtual_items;
}

LayoutUnit MasonryLayoutAlgorithm::ContributionSizeForVirtualItem(
    const GridLayoutTrackCollection& track_collection,
    GridItemContributionType contribution_type,
    GridItemData* virtual_item) const {
  DCHECK(virtual_item);
  DCHECK(virtual_item->contribution_sizes);

  switch (contribution_type) {
    case GridItemContributionType::kForContentBasedMinimums:
    case GridItemContributionType::kForIntrinsicMaximums:
      return virtual_item->contribution_sizes->min_max_contribution.min_size;
    case GridItemContributionType::kForIntrinsicMinimums: {
      const GridTrackSizingDirection track_direction =
          track_collection.Direction();

      // See https://drafts.csswg.org/css-grid/#min-size-auto for more details
      // on the special logic applied for intrinsic minimums.
      if (!virtual_item->IsSpanningAutoMinimumTrack(track_direction) ||
          (virtual_item->IsSpanningFlexibleTrack(track_direction) &&
           virtual_item->SpanSize(track_direction) > 1)) {
        // Per the spec, we apply the automatic min when:
        // - it spans at least one track in that axis whose min track sizing
        // function is auto.
        // - if it spans more than one track in that axis, none of those tracks
        // are flexible.
        return virtual_item->contribution_sizes
            ->intrinsic_min_assuming_track_placement;
      } else {
        // When we aren't spanning tracks that force all items to their
        // automatic minimum, we end up with some items that use the automatic
        // min, and some that use the content minimum. Those that use a content
        // min need to be further clamped by the total track sizes it spans, if
        // those tracks are definite. After clamping, use the max of these two
        // values as the final contribution size.
        const LayoutUnit contribution_unclamped =
            virtual_item->contribution_sizes
                ->intrinsic_min_ignoring_track_placement_unclamped;

        LayoutUnit contribution_to_clamp =
            virtual_item->contribution_sizes
                ->intrinsic_min_ignoring_track_placement;

        const auto& [begin_set_index, end_set_index] =
            virtual_item->SetIndices(track_direction);
        auto spanned_tracks_definite_max_size =
            track_collection.CalculateSetSpanSize(begin_set_index,
                                                  end_set_index);
        if (spanned_tracks_definite_max_size != kIndefiniteSize) {
          contribution_to_clamp = ClampIntrinsicMinSize(
              contribution_to_clamp,
              virtual_item->contribution_sizes->min_clamp_size,
              spanned_tracks_definite_max_size);
        }

        return max(contribution_to_clamp, contribution_unclamped);
      }
    }
    case GridItemContributionType::kForMaxContentMaximums:
    case GridItemContributionType::kForMaxContentMinimums:
      return virtual_item->contribution_sizes->min_max_contribution.max_size;
    case GridItemContributionType::kForFreeSpace:
      NOTREACHED() << "`kForFreeSpace` should only be used to distribute extra "
                      "space in maximize tracks and stretch auto tracks steps.";
  }
}

// TODO(almaher): Eventually look into consolidating repeated code with
// GridLayoutAlgorithm::ContributionSizeForGridItem().
LayoutUnit MasonryLayoutAlgorithm::ComputeMasonryItemBlockContribution(
    GridTrackSizingDirection track_direction,
    SizingConstraint sizing_constraint,
    const ConstraintSpace space_for_measure,
    const GridItemData* masonry_item,
    const bool needs_intrinsic_track_size) const {
  DCHECK(masonry_item);

  // TODO(ikilpatrick): We'll need to record if any child used an indefinite
  // size for its contribution, such that we can then do the 2nd pass on the
  // track-sizing algorithm.

  // TODO(almaher): Handle baseline logic here.

  // TODO(ikilpatrick): This should try and skip layout when possible. Notes:
  //  - We'll need to do a full layout for tables.
  //  - We'll need special logic for replaced elements.
  //  - We'll need to respect the aspect-ratio when appropriate.

  // TODO(almaher): Properly handle submasonry here.

  const LayoutResult* result = nullptr;
  if (space_for_measure.AvailableSize().inline_size == kIndefiniteSize) {
    // If we are orthogonal virtual item, resolving against an indefinite
    // size, set our inline size to our min-content or max-content contribution
    // size depending on the `sizing_contraint`.
    const MinMaxSizes sizes = ComputeMinAndMaxContentContributionForSelf(
                                  masonry_item->node, space_for_measure)
                                  .sizes;
    const auto fallback_space = CreateConstraintSpaceForMeasure(
        *masonry_item,
        /*opt_fixed_inline_size=*/sizing_constraint ==
                SizingConstraint::kMinContent
            ? sizes.min_size
            : sizes.max_size);

    result = LayoutMasonryItemForMeasure(*masonry_item, fallback_space,
                                         sizing_constraint);
  } else {
    result = LayoutMasonryItemForMeasure(*masonry_item, space_for_measure,
                                         sizing_constraint);
  }

  LogicalBoxFragment baseline_fragment(
      masonry_item->BaselineWritingDirection(track_direction),
      To<PhysicalBoxFragment>(result->GetPhysicalFragment()));

  // TODO(almaher): Properly handle baselines here.

  return baseline_fragment.BlockSize();
}

GridSizingTrackCollection MasonryLayoutAlgorithm::ComputeGridAxisTracks(
    const SizingConstraint sizing_constraint,
    const Vector<LayoutUnit>* intrinsic_repeat_track_sizes,
    GridItems& masonry_items,
    Vector<wtf_size_t>& collapsed_track_indexes,
    wtf_size_t& start_offset,
    bool& needs_intrinsic_track_size,
    HeapVector<Member<LayoutBox>>* opt_oof_children) const {
  start_offset = 0;
  needs_intrinsic_track_size = false;

  const GridLineResolver line_resolver(
      Style(), ComputeAutomaticRepetitions(intrinsic_repeat_track_sizes,
                                           needs_intrinsic_track_size));
  const auto& node = Node();
  if (masonry_items.IsEmpty()) {
    masonry_items = node.ConstructMasonryItems(line_resolver, opt_oof_children);
  } else {
    // If `masonry_items` is not empty, that means that we are in
    // a second track sizing pass required for intrinsic tracks within
    // a repeat() track definition. Don't construct the masonry items
    // from scratch. Rather, adjust their spans based on the updated
    // `line_resolver`.
    node.AdjustMasonryItemSpans(masonry_items, line_resolver);
  }

  return BuildGridAxisTracks(line_resolver, masonry_items, sizing_constraint,
                             needs_intrinsic_track_size,
                             collapsed_track_indexes, start_offset);
}

GridSizingTrackCollection MasonryLayoutAlgorithm::BuildGridAxisTracks(
    const GridLineResolver& line_resolver,
    const GridItems& masonry_items,
    SizingConstraint sizing_constraint,
    bool& needs_intrinsic_track_size,
    Vector<wtf_size_t>& collapsed_track_indexes,
    wtf_size_t& start_offset) const {
  const auto& style = Style();
  const auto grid_axis_direction = style.MasonryTrackSizingDirection();
  GridItems virtual_items = BuildVirtualMasonryItems(
      line_resolver, masonry_items, needs_intrinsic_track_size,
      sizing_constraint, line_resolver.AutoRepetitions(grid_axis_direction),
      start_offset);

  // Cache data for DevTools inspector highlighting.
  if (!needs_intrinsic_track_size) {
    GridPlacementData placement_data(line_resolver);
    if (grid_axis_direction == kForColumns) {
      placement_data.column_start_offset = start_offset;
    } else {
      placement_data.row_start_offset = start_offset;
    }
    To<LayoutMasonry>(Node().GetLayoutBox())
        ->SetCachedPlacementData(std::move(placement_data));
  }

  auto BuildRanges = [&]() {
    GridRangeBuilder range_builder(
        style, grid_axis_direction,
        line_resolver.AutoRepetitions(grid_axis_direction), start_offset);

    for (auto& virtual_item : virtual_items) {
      auto& range_indices = virtual_item.RangeIndices(grid_axis_direction);
      const auto& span = virtual_item.Span(grid_axis_direction);

      range_builder.EnsureTrackCoverage(span.StartLine(), span.IntegerSpan(),
                                        &range_indices.begin,
                                        &range_indices.end);
    }
    return range_builder.FinalizeRanges(needs_intrinsic_track_size,
                                        &collapsed_track_indexes);
  };

  GridSizingTrackCollection track_collection(BuildRanges(),
                                             grid_axis_direction);
  track_collection.BuildSets(style, masonry_available_size_);

  if (track_collection.HasNonDefiniteTrack()) {
    GridTrackSizingAlgorithm::CacheGridItemsProperties(track_collection,
                                                       &virtual_items);

    const GridTrackSizingAlgorithm track_sizing_algorithm(
        style, masonry_available_size_, masonry_min_available_size_,
        sizing_constraint);

    track_collection.CacheInitializedSetsGeometry(
        (grid_axis_direction == kForColumns)
            ? BorderScrollbarPadding().inline_start
            : BorderScrollbarPadding().block_start);

    // TODO(almaher): Once we introduce the sizing subtree for subgrid,
    // we can use that to get the track collection, similar to grid.
    track_sizing_algorithm.ComputeUsedTrackSizes(
        [&](GridItemContributionType contribution_type,
            GridItemData* virtual_item) {
          return ContributionSizeForVirtualItem(
              track_collection, contribution_type, virtual_item);
        },
        &track_collection, &virtual_items, needs_intrinsic_track_size);
  }

  auto first_set_geometry = GridTrackSizingAlgorithm::ComputeFirstSetGeometry(
      track_collection, style, masonry_available_size_,
      BorderScrollbarPadding());

  track_collection.FinalizeSetsGeometry(first_set_geometry.start_offset,
                                        first_set_geometry.gutter_size);

  return track_collection;
}

Vector<LayoutUnit> MasonryLayoutAlgorithm::GetIntrinsicRepeaterTrackSizes(
    bool has_items,
    const GridSizingTrackCollection& track_collection) const {
  CHECK_NE(track_collection.GetIntrinsicSizedRepeaterSetIndex(), kNotFound);
  const ComputedStyle& style = Style();
  const bool is_for_columns =
      style.MasonryTrackSizingDirection() == kForColumns;

  const GridTrackList& track_list =
      is_for_columns ? style.GridTemplateColumns().GetTrackList()
                     : style.GridTemplateRows().GetTrackList();
  const wtf_size_t repeat_track_count = track_list.AutoRepeatTrackCount();

  Vector<LayoutUnit> intrinsic_repeat_track_sizes(repeat_track_count);

  if (!has_items) {
    // If there are no items, the size of the intrinsic tracks within an auto
    // repeat are zero.
    return intrinsic_repeat_track_sizes;
  }

  for (wtf_size_t i = 0; i < repeat_track_count; ++i) {
    GridSet current_set = track_collection.GetSetAt(
        track_collection.GetIntrinsicSizedRepeaterSetIndex() + i);

    // During the first pass to calculate the intrinsic repeater track
    // sizes, we consolidate all spanners to a single span and place
    // the largest contribution in every track position, which will
    // guarantee that each set will have a single track.
    CHECK_EQ(current_set.track_count, 1U);

    // Note that when `needs_intrinsic_track_size` is true, we skip the
    // steps to distribute free space during track sizing. This means that
    // the base track size at this point represents the size of the
    // intrinsic track without free space distribution and hasn't taken the
    // growth limit into account.
    intrinsic_repeat_track_sizes[i] = current_set.GrowthLimit();
  }
  return intrinsic_repeat_track_sizes;
}

// https://drafts.csswg.org/css-grid-2/#auto-repeat
wtf_size_t MasonryLayoutAlgorithm::ComputeAutomaticRepetitions(
    const Vector<LayoutUnit>* intrinsic_repeat_track_sizes,
    bool& needs_intrinsic_track_size) const {
  const ComputedStyle& style = Style();
  GridTrackSizingDirection masonry_track_sizing_direction =
      style.MasonryTrackSizingDirection();
  const bool is_for_columns = masonry_track_sizing_direction == kForColumns;

  const GridTrackList& track_list =
      is_for_columns ? style.GridTemplateColumns().GetTrackList()
                     : style.GridTemplateRows().GetTrackList();

  if (!track_list.HasAutoRepeater()) {
    return 0;
  }

  // To determine an intrinsic track size within a repeat, we need to expand
  // them out once, and run track sizing to get the actual size [1]. Then we
  // will run this again with the actual intrinsic track size within a final
  // track sizing pass based on this size.
  //
  // https://www.w3.org/TR/css-grid-3/#masonry-intrinsic-repeat
  if (track_list.HasIntrinsicSizedRepeater() && !intrinsic_repeat_track_sizes) {
    CHECK(!needs_intrinsic_track_size);
    needs_intrinsic_track_size = true;
    return 1;
  }

  // TODO(almaher): We will need special computation of automatic repetitions
  // for submasonry (see ComputeAutomaticRepetitionsForSubgrid()). Once this is
  // supported, we can move more of this method to the helper in
  // grid_layout_utils.cc.

  const LayoutUnit gutter_size = GridTrackSizingAlgorithm::CalculateGutterSize(
      style, masonry_available_size_, masonry_track_sizing_direction);

  return CalculateAutomaticRepetitions(
      track_list, gutter_size,
      is_for_columns ? masonry_available_size_.inline_size
                     : masonry_available_size_.block_size,
      is_for_columns ? masonry_min_available_size_.inline_size
                     : masonry_min_available_size_.block_size,
      is_for_columns ? masonry_max_available_size_.inline_size
                     : masonry_max_available_size_.block_size,
      intrinsic_repeat_track_sizes);
}

ConstraintSpace MasonryLayoutAlgorithm::CreateConstraintSpace(
    const GridItemData& masonry_item,
    const LogicalSize& containing_size,
    const LogicalSize& fixed_available_size,
    LayoutResultCacheSlot result_cache_slot) const {
  ConstraintSpaceBuilder builder(
      GetConstraintSpace(), masonry_item.node.Style().GetWritingDirection(),
      /*is_new_fc=*/true, /*adjust_inline_size_if_needed=*/false);

  builder.SetCacheSlot(result_cache_slot);
  builder.SetIsPaintedAtomically(true);

  {
    LogicalSize available_size = containing_size;
    if (fixed_available_size.inline_size != kIndefiniteSize) {
      available_size.inline_size = fixed_available_size.inline_size;
      builder.SetIsFixedInlineSize(true);
    }

    if (fixed_available_size.block_size != kIndefiniteSize) {
      available_size.block_size = fixed_available_size.block_size;
      builder.SetIsFixedBlockSize(true);
    }
    builder.SetAvailableSize(available_size);
  }

  builder.SetPercentageResolutionSize(containing_size);
  builder.SetInlineAutoBehavior(masonry_item.column_auto_behavior);
  builder.SetBlockAutoBehavior(masonry_item.row_auto_behavior);
  return builder.ToConstraintSpace();
}

// TODO(celestepan): If item-direction is row, we should not be returning an
// indefinite inline size. Discussions are still ongoing on if we want to always
// return min/max-content or inherit from the parent.
ConstraintSpace MasonryLayoutAlgorithm::CreateConstraintSpaceForLayout(
    const GridItemData& masonry_item,
    const GridLayoutTrackCollection& track_collection,
    std::optional<LayoutUnit> opt_fixed_inline_size,
    LogicalRect* containing_rect) const {
  const bool is_for_columns = track_collection.Direction() == kForColumns;

  auto containing_size = masonry_available_size_;
  auto& grid_axis_size =
      is_for_columns ? containing_size.inline_size : containing_size.block_size;

  LayoutUnit start_offset;
  grid_axis_size =
      masonry_item.CalculateAvailableSize(track_collection, &start_offset);

  if (containing_rect) {
    is_for_columns ? containing_rect->offset.inline_offset = start_offset
                   : containing_rect->offset.block_offset = start_offset;
    containing_rect->size = containing_size;
  }

  // Unlike grid, in masonry, we are only contrained by the final track sizing
  // in one dimension. However, at track sizing, we may force a block/inline
  // constraint for orthogonal items. This logic ensures we enforce the same
  // constraint at layout, as well. Otherwise, we can end up with odd layout and
  // overflow of items that we don't get in grid.
  LogicalSize fixed_available_size = kIndefiniteLogicalSize;
  if (opt_fixed_inline_size) {
    const auto writing_mode = GetConstraintSpace().GetWritingMode();
    const auto item_writing_mode = masonry_item.node.Style().GetWritingMode();
    const bool is_parallel =
        IsParallelWritingMode(item_writing_mode, writing_mode);
    const bool used_block_constraint_at_track_sizing =
        is_for_columns ? !is_parallel : is_parallel;
    if (used_block_constraint_at_track_sizing) {
      if (is_parallel) {
        if (containing_size.inline_size == kIndefiniteSize) {
          CHECK_NE(containing_size.block_size, kIndefiniteSize);
          fixed_available_size.inline_size = *opt_fixed_inline_size;
        }
      } else {
        if (containing_size.block_size == kIndefiniteSize) {
          CHECK_NE(containing_size.inline_size, kIndefiniteSize);
          fixed_available_size.block_size = *opt_fixed_inline_size;
        }
      }
    }
  }

  // TODO(almaher): Will likely need special fixed available size handling for
  // submasonry.
  return CreateConstraintSpace(masonry_item, containing_size,
                               fixed_available_size,
                               LayoutResultCacheSlot::kLayout);
}

ConstraintSpace MasonryLayoutAlgorithm::CreateConstraintSpaceForMeasure(
    const GridItemData& masonry_item,
    std::optional<LayoutUnit> opt_fixed_inline_size,
    bool is_for_min_max_sizing) const {
  LogicalSize containing_size = masonry_available_size_;
  const auto writing_mode = GetConstraintSpace().GetWritingMode();
  const auto grid_axis_direction = Style().MasonryTrackSizingDirection();

  // Check against columns, as opposed to whether the item is parallel, because
  // the ConstraintSpaceBuilder takes care of handling orthogonal items.
  if (grid_axis_direction == kForColumns) {
    containing_size.inline_size = kIndefiniteSize;
  } else {
    if (is_for_min_max_sizing) {
      // In the row direction, we use this method to create a space for
      // measuring the min/max-content of the item, so we have to set the inline
      // size as indefinite to allow for text flow.
      containing_size.inline_size = kIndefiniteSize;
    }
    containing_size.block_size = kIndefiniteSize;
  }

  // TODO(almaher): Do we need to do something special here for subgrid like
  // GridLayoutAlgorithm::CreateConstraintSpaceForMeasure()?
  LogicalSize fixed_available_size = kIndefiniteLogicalSize;

  if (opt_fixed_inline_size) {
    const auto item_writing_mode = masonry_item.node.Style().GetWritingMode();
    if (IsParallelWritingMode(item_writing_mode, writing_mode)) {
      DCHECK_EQ(fixed_available_size.inline_size, kIndefiniteSize);
      fixed_available_size.inline_size = *opt_fixed_inline_size;
    } else {
      DCHECK_EQ(fixed_available_size.block_size, kIndefiniteSize);
      fixed_available_size.block_size = *opt_fixed_inline_size;
    }
  }

  return CreateConstraintSpace(masonry_item, containing_size,
                               fixed_available_size,
                               LayoutResultCacheSlot::kMeasure);
}

// static
LogicalRect MasonryLayoutAlgorithm::ComputeOutOfFlowItemContainingRect(
    const GridPlacementData& placement_data,
    const GridLayoutData& layout_data,
    const ComputedStyle& masonry_style,
    const BoxStrut& borders,
    const LogicalSize& border_box_size,
    const BoxStrut& border_scrollbar_padding,
    GridItemData* out_of_flow_item) {
  DCHECK(out_of_flow_item && out_of_flow_item->IsOutOfFlow());

  // Compute the containing rect for out-of-flow items in masonry:
  // - Grid axis: Use the item's grid-area placement (similar to CSS Grid)
  // - Stacking axis: Use the full container size minus
  // border/scrollbar/padding, since items flow and stack naturally in this
  // direction and out-of-flow items should have access to the entire available
  // space.

  const bool is_for_columns =
      masonry_style.MasonryTrackSizingDirection() == kForColumns;

  out_of_flow_item->ComputeOutOfFlowItemPlacement(
      is_for_columns ? layout_data.Columns() : layout_data.Rows(),
      placement_data, masonry_style);
  LogicalRect containing_rect;

  if (is_for_columns) {
    ComputeOutOfFlowOffsetAndSize(*out_of_flow_item, layout_data.Columns(),
                                  borders, border_box_size,
                                  &containing_rect.offset.inline_offset,
                                  &containing_rect.size.inline_size);

    containing_rect.offset.block_offset = border_scrollbar_padding.block_start;
    containing_rect.size.block_size =
        border_box_size.block_size - border_scrollbar_padding.BlockSum();
  } else {
    ComputeOutOfFlowOffsetAndSize(
        *out_of_flow_item, layout_data.Rows(), borders, border_box_size,
        &containing_rect.offset.block_offset, &containing_rect.size.block_size);

    containing_rect.offset.inline_offset =
        border_scrollbar_padding.inline_start;
    containing_rect.size.inline_size =
        border_box_size.inline_size - border_scrollbar_padding.InlineSum();
  }

  return containing_rect;
}

}  // namespace blink
