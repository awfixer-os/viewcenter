// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/flex/flex_gap_accumulator.h"

#include "third_party/blink/renderer/core/layout/box_fragment_builder.h"
#include "third_party/blink/renderer/core/layout/gap/gap_geometry.h"

namespace blink {

const GapGeometry* FlexGapAccumulator::BuildGapGeometry() {
  const bool has_valid_main_axis_gaps =
      !main_gaps_.empty() && gap_between_lines_ > LayoutUnit();
  const bool has_valid_cross_axis_gaps =
      !cross_gaps_.empty() && gap_between_items_ > LayoutUnit();
  if (!has_valid_main_axis_gaps && !has_valid_cross_axis_gaps) {
    // `GapGeometry` requires at least one axis to be valid.
    return nullptr;
  }

  GapGeometry* gap_geometry =
      MakeGarbageCollected<GapGeometry>(GapGeometry::ContainerType::kFlex);

  if (is_column_) {
    // In a column flex container, the main axis gaps become the "columns" and
    // the cross axis gaps become the "rows".
    if (gap_between_lines_ > LayoutUnit()) {
      gap_geometry->SetInlineGapSize(gap_between_lines_);
    }
    if (gap_between_items_ > LayoutUnit()) {
      gap_geometry->SetBlockGapSize(gap_between_items_);
    }

    gap_geometry->SetMainDirection(kForColumns);
  } else {
    if (gap_between_lines_ > LayoutUnit()) {
      gap_geometry->SetBlockGapSize(gap_between_lines_);
    }
    if (gap_between_items_ > LayoutUnit()) {
      gap_geometry->SetInlineGapSize(gap_between_items_);
    }
  }

  // TODO(crbug.com/436140061): The following are for the optimized
  // version of GapDecorations. Once the optimized version is implemented,
  // we can remove all the parts of this function used for the old version.
  // TODO(crbug.com/440123087): Risky since they could in theory be used after
  // moved. Clean up to not move members. Change members to unique_ptrs
  if (!cross_gaps_.empty()) {
    gap_geometry->SetCrossGaps(std::move(cross_gaps_));
  }

  if (!main_gaps_.empty()) {
    gap_geometry->SetMainGaps(std::move(main_gaps_));
  }

  LayoutUnit content_inline_start =
      is_column_ ? content_cross_start_ : content_main_start_;
  LayoutUnit content_inline_end =
      is_column_ ? content_cross_end_ : content_main_end_;
  LayoutUnit content_block_start =
      is_column_ ? content_main_start_ : content_cross_start_;
  LayoutUnit content_block_end =
      is_column_ ? content_main_end_ : content_cross_end_;

  gap_geometry->SetContentInlineOffsets(content_inline_start,
                                        content_inline_end);
  gap_geometry->SetContentBlockOffsets(content_block_start, content_block_end);

  return gap_geometry;
}

void FlexGapAccumulator::BuildGapsForCurrentItem(
    const FlexLineVector& flex_lines,
    wtf_size_t flex_line_index,
    wtf_size_t item_index_in_line,
    LogicalOffset item_offset,
    bool is_first_line,
    bool is_last_line,
    LayoutUnit line_cross_start,
    LayoutUnit line_cross_end) {
  CHECK_LT(flex_line_index, flex_lines.size());
  const FlexLine& flex_line = flex_lines[flex_line_index];

  // "first" and "last" here refers to the inline direction.
  const bool is_first_item = item_index_in_line == 0;
  const bool is_last_item =
      item_index_in_line == flex_line.item_indices.size() - 1;

  const bool single_line = is_first_line && is_last_line;

  if (is_first_line && is_first_item) {
    content_cross_start_ = line_cross_start;
    content_main_start_ =
        is_column_ ? container_builder_->BorderScrollbarPadding().block_start
                   : container_builder_->BorderScrollbarPadding().inline_start;
    const LayoutUnit main_offset =
        is_column_ ? item_offset.block_offset : item_offset.inline_offset;
    content_main_start_ = std::min(content_main_start_, main_offset);
  }

  if (is_last_line && is_first_item) {
    content_cross_end_ = line_cross_end;
  }

  // The first item in any line doesn't have any `CrossGap` associated with
  // it.
  if (is_first_item) {
    // We set the `MainGap` start offset when we process the first item in a
    // line, and nothing else. The last line does not have any `MainGap`s.
    if (!single_line && !is_last_line) {
      PopulateMainGapForFirstItem(line_cross_end);

      if (flex_line.item_indices.size() == 1) {
        LayoutUnit border_scrollbar_padding =
            is_column_
                ? container_builder_->BorderScrollbarPadding().block_end
                : container_builder_->BorderScrollbarPadding().inline_end;
        LayoutUnit container_main_end =
            is_column_
                ? container_builder_->InitialBorderBoxSize().block_size -
                      border_scrollbar_padding
                : container_builder_->InlineSize() - border_scrollbar_padding;
        content_main_end_ = container_main_end;
      }
      return;
    }
    return;
  }

  const LayoutUnit main_offset =
      is_column_ ? item_offset.block_offset : item_offset.inline_offset;
  const LayoutUnit main_intersection_offset =
      main_offset - (gap_between_items_ / 2);

  PopulateCrossGapForCurrentItem(flex_line, flex_line_index, is_first_line,
                                 is_last_line, single_line,
                                 main_intersection_offset, line_cross_start);

  if (is_last_item) {
    LayoutUnit border_scrollbar_padding =
        is_column_ ? container_builder_->BorderScrollbarPadding().block_end
                   : container_builder_->BorderScrollbarPadding().inline_end;
    LayoutUnit container_main_end =
        is_column_
            ? container_builder_->InitialBorderBoxSize().block_size -
                  border_scrollbar_padding
            : container_builder_->InlineSize() - border_scrollbar_padding;

    const LayoutUnit last_gap_offset =
        is_column_ ? cross_gaps_.back().GetGapOffset().block_offset
                   : cross_gaps_.back().GetGapOffset().inline_offset;
    content_main_end_ = std::max(last_gap_offset, container_main_end);
  }
}

void FlexGapAccumulator::PopulateMainGapForFirstItem(LayoutUnit cross_end) {
  LayoutUnit gap_offset = cross_end + (gap_between_lines_ / 2);
  main_gaps_.emplace_back(gap_offset);
}

void FlexGapAccumulator::HandleCrossGapRangesForCurrentItem(
    wtf_size_t flex_line_index,
    wtf_size_t cross_gap_index) {
  if (main_gaps_.empty()) {
    return;
  }

  if (flex_line_index < main_gaps_.size()) {
    main_gaps_[flex_line_index].IncrementRangeOfCrossGapsBefore(
        cross_gap_index);
  }

  if (flex_line_index > 0) {
    CHECK_LE(flex_line_index - 1, main_gaps_.size());
    // We increment the `RangeOfCrossGapsAfter` for the previous line, since
    // the CrossGaps that start at this line fall "after" the previous line.
    main_gaps_[flex_line_index - 1].IncrementRangeOfCrossGapsAfter(
        cross_gap_index);
  }
}

void FlexGapAccumulator::PopulateCrossGapForCurrentItem(
    const FlexLine& flex_line,
    wtf_size_t flex_line_index,
    bool is_first_line,
    bool is_last_line,
    bool single_line,
    LayoutUnit main_intersection_offset,
    LayoutUnit cross_start) {
  // If we are in the first or last flex line, our the `CrossGap` associated
  // with this item will start at the point given by
  // `main_intersection_offset`, and the either cross axis of the line or the
  // cross axis offset of the line minus half of the gap size.
  //
  // If we are in the middle flex line, the `CrossGap` associated with this
  // item will start at the point given by `main_intersection_offset`, and the
  // midpoint between the start of the line and the end of the last line.

  LayoutUnit cross_intersection_offset = cross_start;
  CrossGap::EdgeIntersectionState edge_state =
      CrossGap::EdgeIntersectionState::kNone;

  if (single_line) {
    // If there is only one line, the cross gap will start and end at the
    // content edge.
    edge_state = CrossGap::EdgeIntersectionState::kBoth;
  } else if (is_first_line) {
    // First line, so the cross gap starts at the content edge.
    edge_state = CrossGap::EdgeIntersectionState::kStart;
  } else if (is_last_line) {
    // If there is more than one flex line, and the current line is the last
    // line, the cross offset will be the cross axis offset of the line
    // minus half of the gap size.
    cross_intersection_offset -= gap_between_lines_ / 2;
    edge_state = CrossGap::EdgeIntersectionState::kEnd;
  } else {
    // Middle line, so the cross gap will start at midpoint between the start
    // of this line and the end of the previous line.
    cross_intersection_offset =
        flex_line.cross_axis_offset - (gap_between_lines_ / 2);
  }

  LogicalOffset logical_offset(
      is_column_ ? cross_intersection_offset : main_intersection_offset,
      is_column_ ? main_intersection_offset : cross_intersection_offset);
  CrossGap cross_gap(logical_offset, edge_state);

  cross_gaps_.push_back(cross_gap);
  HandleCrossGapRangesForCurrentItem(flex_line_index, cross_gaps_.size() - 1);
}

}  // namespace blink
