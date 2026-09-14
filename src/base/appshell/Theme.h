// Theme — 两款 exe 共用的现代深色界面主题。
// 替代 ImGui::StyleColorsDark：统一配色（蓝灰底 + 品牌蓝强调）、圆角、间距与停靠标签样式。
#pragma once

namespace pv::theme {

// 应用现代深色主题到当前 ImGui 上下文（ImGui 初始化后、首帧前调用一次）
void applyModern();

} // namespace pv::theme
