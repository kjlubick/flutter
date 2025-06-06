// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/utils/dl_matrix_clip_tracker.h"

#include "flutter/display_list/dl_builder.h"
#include "flutter/fml/logging.h"
#include "flutter/impeller/geometry/round_superellipse_param.h"

namespace flutter {

static constexpr DlRect kEmpty = DlRect();

static const DlRect& ProtectEmpty(const DlRect& rect) {
  // isEmpty protects us against NaN while we normalize any empty cull rects
  return rect.IsEmpty() ? kEmpty : rect;
}

DisplayListMatrixClipState::DisplayListMatrixClipState(const DlRect& cull_rect,
                                                       const DlMatrix& matrix)
    : cull_rect_(ProtectEmpty(cull_rect)), matrix_(matrix) {}

bool DisplayListMatrixClipState::inverseTransform(
    const DisplayListMatrixClipState& tracker) {
  if (tracker.is_matrix_invertable()) {
    matrix_ = matrix_ * tracker.matrix_.Invert();
    return true;
  }
  return false;
}

bool DisplayListMatrixClipState::mapAndClipRect(const DlRect& src,
                                                DlRect* mapped) const {
  DlRect dl_mapped = src.TransformAndClipBounds(matrix_);
  auto dl_intersected = dl_mapped.Intersection(cull_rect_);
  if (dl_intersected.has_value()) {
    *mapped = dl_intersected.value();
    return true;
  }
  *mapped = DlRect();
  return false;
}

void DisplayListMatrixClipState::clipRect(const DlRect& rect,
                                          DlClipOp op,
                                          bool is_aa) {
  if (rect.IsFinite()) {
    adjustCullRect(rect, op, is_aa);
  }
}

void DisplayListMatrixClipState::clipOval(const DlRect& bounds,
                                          DlClipOp op,
                                          bool is_aa) {
  if (!bounds.IsFinite()) {
    return;
  }
  switch (op) {
    case DlClipOp::kIntersect:
      adjustCullRect(bounds, op, is_aa);
      break;
    case DlClipOp::kDifference:
      if (oval_covers_cull(bounds)) {
        cull_rect_ = DlRect();
      }
      break;
  }
}

namespace {
inline std::array<DlRect, 2> RoundingRadiiSafeRects(
    const DlRect& bounds,
    const impeller::RoundingRadii& radii) {
  return {
      bounds.Expand(  //
          -std::max(radii.top_left.width, radii.bottom_left.width), 0,
          -std::max(radii.top_right.width, radii.bottom_right.width), 0),
      bounds.Expand(
          0, -std::max(radii.top_left.height, radii.top_right.height),  //
          0, -std::max(radii.bottom_left.height, radii.bottom_right.height))};
}
}  // namespace

void DisplayListMatrixClipState::clipRRect(const DlRoundRect& rrect,
                                           DlClipOp op,
                                           bool is_aa) {
  DlRect bounds = rrect.GetBounds();
  if (rrect.IsRect()) {
    return clipRect(bounds, op, is_aa);
  }
  if (rrect.IsOval()) {
    return clipOval(bounds, op, is_aa);
  }
  switch (op) {
    case DlClipOp::kIntersect:
      adjustCullRect(bounds, op, is_aa);
      break;
    case DlClipOp::kDifference: {
      if (rrect_covers_cull(rrect)) {
        cull_rect_ = DlRect();
        return;
      }
      auto safe_rects = RoundingRadiiSafeRects(bounds, rrect.GetRadii());
      adjustCullRect(safe_rects[0], op, is_aa);
      adjustCullRect(safe_rects[1], op, is_aa);
      break;
    }
  }
}

void DisplayListMatrixClipState::clipRSuperellipse(
    const DlRoundSuperellipse& rse,
    DlClipOp op,
    bool is_aa) {
  DlRect bounds = rse.GetBounds();
  if (rse.IsRect()) {
    return clipRect(bounds, op, is_aa);
  }
  if (rse.IsOval()) {
    return clipOval(bounds, op, is_aa);
  }
  switch (op) {
    case DlClipOp::kIntersect:
      adjustCullRect(bounds, op, is_aa);
      break;
    case DlClipOp::kDifference: {
      if (rsuperellipse_covers_cull(rse)) {
        cull_rect_ = DlRect();
        return;
      }
      auto safe_rects = RoundingRadiiSafeRects(bounds, rse.GetRadii());
      adjustCullRect(safe_rects[0], op, is_aa);
      adjustCullRect(safe_rects[1], op, is_aa);
      break;
    }
  }
}

void DisplayListMatrixClipState::clipPath(const DlPath& path,
                                          DlClipOp op,
                                          bool is_aa) {
  DlRect bounds = path.GetBounds();
  if (path.IsRect(nullptr)) {
    return clipRect(bounds, op, is_aa);
  }
  switch (op) {
    case DlClipOp::kIntersect:
      adjustCullRect(bounds, op, is_aa);
      break;
    case DlClipOp::kDifference:
      break;
  }
}

bool DisplayListMatrixClipState::content_culled(
    const DlRect& content_bounds) const {
  if (cull_rect_.IsEmpty() || content_bounds.IsEmpty()) {
    return true;
  }
  if (!is_matrix_invertable()) {
    return true;
  }
  if (has_perspective()) {
    return false;
  }
  DlRect mapped;
  mapRect(content_bounds, &mapped);
  return !mapped.IntersectsWithRect(cull_rect_);
}

void DisplayListMatrixClipState::resetDeviceCullRect(const DlRect& cull_rect) {
  if (cull_rect.IsEmpty()) {
    cull_rect_ = DlRect();
  } else {
    cull_rect_ = cull_rect;
  }
}

void DisplayListMatrixClipState::resetLocalCullRect(const DlRect& cull_rect) {
  if (!cull_rect.IsEmpty()) {
    mapRect(cull_rect, &cull_rect_);
    if (!cull_rect_.IsEmpty()) {
      return;
    }
  }
  cull_rect_ = DlRect();
}

void DisplayListMatrixClipState::adjustCullRect(const DlRect& clip,
                                                DlClipOp op,
                                                bool is_aa) {
  if (cull_rect_.IsEmpty()) {
    // No point in constraining further.
    return;
  }
  if (matrix_.HasPerspective()) {
    // We can conservatively ignore this clip.
    return;
  }
  switch (op) {
    case DlClipOp::kIntersect: {
      if (clip.IsEmpty()) {
        cull_rect_ = DlRect();
        break;
      }
      DlRect rect;
      mapRect(clip, &rect);
      if (is_aa) {
        rect = DlRect::RoundOut(rect);
      }
      cull_rect_ = cull_rect_.Intersection(rect).value_or(DlRect());
      break;
    }
    case DlClipOp::kDifference: {
      if (clip.IsEmpty()) {
        break;
      }
      DlRect rect;
      if (mapRect(clip, &rect)) {
        // This technique only works if the transform is rect -> rect
        if (is_aa) {
          rect = DlRect::Round(rect);
          if (rect.IsEmpty()) {
            break;
          }
        }
        cull_rect_ = cull_rect_.CutoutOrEmpty(rect);
      }
      break;
    }
  }
}

DlRect DisplayListMatrixClipState::GetLocalCullCoverage() const {
  if (cull_rect_.IsEmpty()) {
    return DlRect();
  }
  if (!is_matrix_invertable()) {
    return DlRect();
  }
  if (matrix_.HasPerspective2D()) {
    // We could do a 4-point long-form conversion, but since this is
    // only used for culling, let's just return a non-constricting
    // cull rect.
    return DisplayListBuilder::kMaxCullRect;
  }
  DlMatrix inverse = matrix_.Invert();
  // We eliminated perspective above so we can use the cheaper non-clipping
  // bounds transform method.
  return cull_rect_.TransformBounds(inverse);
}

bool DisplayListMatrixClipState::rect_covers_cull(const DlRect& content) const {
  return TransformedRectCoversBounds(content, matrix_, cull_rect_);
}

bool DisplayListMatrixClipState::TransformedRectCoversBounds(
    const DlRect& local_rect,
    const DlMatrix& matrix,
    const DlRect& cull_bounds) {
  if (local_rect.IsEmpty()) {
    return false;
  }
  if (cull_bounds.IsEmpty()) {
    return true;
  }
  if (matrix.IsAligned2D()) {
    // This transform-to-device calculation is faster and more accurate
    // for rect-to-rect aligned transformations, but not accurate under
    // (non-quadrant) rotations and skews.
    return local_rect.TransformAndClipBounds(matrix).Contains(cull_bounds);
  }
  DlPoint corners[4];
  if (!GetLocalCorners(corners, cull_bounds, matrix)) {
    return false;
  }
  for (auto corner : corners) {
    if (!local_rect.ContainsInclusive(corner)) {
      return false;
    }
  }
  return true;
}

bool DisplayListMatrixClipState::oval_covers_cull(const DlRect& bounds) const {
  return TransformedOvalCoversBounds(bounds, matrix_, cull_rect_);
}

bool DisplayListMatrixClipState::TransformedOvalCoversBounds(
    const DlRect& local_oval_bounds,
    const DlMatrix& matrix,
    const DlRect& cull_bounds) {
  if (local_oval_bounds.IsEmpty()) {
    return false;
  }
  if (cull_bounds.IsEmpty()) {
    return true;
  }
  DlPoint corners[4];
  if (!GetLocalCorners(corners, cull_bounds, matrix)) {
    return false;
  }
  DlPoint center = local_oval_bounds.GetCenter();
  DlSize scale = 2.0 / local_oval_bounds.GetSize();
  for (auto corner : corners) {
    if (!local_oval_bounds.Contains(corner)) {
      return false;
    }
    if (((corner - center) * scale).GetLengthSquared() >= 1.0) {
      return false;
    }
  }
  return true;
}

bool DisplayListMatrixClipState::rrect_covers_cull(
    const DlRoundRect& content) const {
  return TransformedRRectCoversBounds(content, matrix_, cull_rect_);
}

bool DisplayListMatrixClipState::TransformedRRectCoversBounds(
    const DlRoundRect& local_rrect,
    const DlMatrix& matrix,
    const DlRect& cull_bounds) {
  if (local_rrect.IsEmpty()) {
    return false;
  }
  if (cull_bounds.IsEmpty()) {
    return true;
  }
  if (local_rrect.IsRect()) {
    return TransformedRectCoversBounds(local_rrect.GetBounds(), matrix,
                                       cull_bounds);
  }
  if (local_rrect.IsOval()) {
    return TransformedOvalCoversBounds(local_rrect.GetBounds(), matrix,
                                       cull_bounds);
  }
  if (!local_rrect.GetRadii().AreAllCornersSame()) {
    return false;
  }
  DlPoint corners[4];
  if (!GetLocalCorners(corners, cull_bounds, matrix)) {
    return false;
  }
  auto outer = local_rrect.GetBounds();
  auto center = outer.GetCenter();
  auto radii = local_rrect.GetRadii().top_left;
  auto inner = outer.GetSize() * 0.5 - radii;
  auto scale = 1.0 / radii;
  for (auto corner : corners) {
    if (!outer.Contains(corner)) {
      return false;
    }
    auto rel = (corner - center).Abs() - inner;
    if (rel.x > 0.0f && rel.y > 0.0f &&
        (rel * scale).GetLengthSquared() >= 1.0f) {
      return false;
    }
  }
  return true;
}

bool DisplayListMatrixClipState::rsuperellipse_covers_cull(
    const DlRoundSuperellipse& content) const {
  return TransformedRoundSuperellipseCoversBounds(content, matrix_, cull_rect_);
}

bool DisplayListMatrixClipState::TransformedRoundSuperellipseCoversBounds(
    const DlRoundSuperellipse& local_rse,
    const DlMatrix& matrix,
    const DlRect& cull_bounds) {
  if (local_rse.IsEmpty()) {
    return false;
  }
  if (cull_bounds.IsEmpty()) {
    return true;
  }
  if (local_rse.IsRect()) {
    return TransformedRectCoversBounds(local_rse.GetBounds(), matrix,
                                       cull_bounds);
  }
  if (local_rse.IsOval()) {
    return TransformedOvalCoversBounds(local_rse.GetBounds(), matrix,
                                       cull_bounds);
  }
  DlPoint corners[4];
  if (!GetLocalCorners(corners, cull_bounds, matrix)) {
    return false;
  }
  auto outer = local_rse.GetBounds();
  auto param = impeller::RoundSuperellipseParam::MakeBoundsRadii(
      outer, local_rse.GetRadii());
  for (auto corner : corners) {
    if (!outer.Contains(corner)) {
      return false;
    }
    if (!param.Contains(corner)) {
      return false;
    }
  }
  return true;
}

bool DisplayListMatrixClipState::GetLocalCorners(DlPoint corners[4],
                                                 const DlRect& rect,
                                                 const DlMatrix& matrix) {
  if (!matrix.IsInvertible()) {
    return false;
  }
  DlMatrix inverse = matrix.Invert();
  corners[0] = inverse * rect.GetLeftTop();
  corners[1] = inverse * rect.GetRightTop();
  corners[2] = inverse * rect.GetRightBottom();
  corners[3] = inverse * rect.GetLeftBottom();
  return true;
}

}  // namespace flutter
