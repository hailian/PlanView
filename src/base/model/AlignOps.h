// AlignOps — 画布组件对齐/尺寸/分布纯几何运算（无 UI 依赖，便于单测）。
// 对齐/尺寸以 ref（参考件，通常为锚点）为基准；分布基于选中集跨度等距。
#pragma once

#include <vector>

#include "base/model/Types.h"

namespace pv {

enum class AlignMode { Left, HCenter, Right, Top, VCenter, Bottom };
enum class SizeMode { Width, Height, Both };

// 就地修改：把每个矩形按 ref 的对应边/中心对齐
void alignRects(const std::vector<Rect*>& rects, const Rect& ref, AlignMode mode);

// 就地修改：把每个矩形尺寸设为 ref 的宽/高
void sizeRects(const std::vector<Rect*>& rects, const Rect& ref, SizeMode mode);

// 就地修改：水平/垂直等距分布（保持首尾两件外缘不动；元素 <3 无操作）
void distributeRects(const std::vector<Rect*>& rects, bool horizontal);

} // namespace pv
