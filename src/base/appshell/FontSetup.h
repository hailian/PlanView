// 中文字体加载：微软雅黑 → 黑体 → 宋体 → ImGui 默认字体的回退链。
// 必须在 ImGui context 创建后、首帧前调用一次。
#pragma once

namespace pv::font {

// 返回实际加载的字体名（用于日志/诊断）。
const char* setupChineseFont(float sizePixels);

} // namespace pv::font
