#include "base/appshell/FontSetup.h"

#include "base/log/Log.h"
#include "imgui.h"

#include <cstdio>

namespace softg::font {

// Win11 必有雅黑；LTSC/精简系统可能缺，逐级回退。
// imgui 1.92 动态字体：按需从字体文件加载字形，无需指定 glyph ranges。
static const char* kCandidates[] = {
    "C:/Windows/Fonts/msyh.ttc",    // 微软雅黑
    "C:/Windows/Fonts/simhei.ttf",  // 黑体
    "C:/Windows/Fonts/simsun.ttc",  // 宋体
};

const char* setupChineseFont(float sizePixels) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : kCandidates) {
        ImFont* font = io.Fonts->AddFontFromFileTTF(path, sizePixels);
        if (font) {
            SOFTG_LOG_INFO("中文字体加载成功: %s (%.0fpx)", path, sizePixels);
            return path;
        }
    }
    // 全部失败：保留 ImGui 内嵌默认字体（无中文，界面会出现 '?'）
    SOFTG_LOG_WARN("未找到中文字体，回退 ImGui 默认字体（中文将无法显示）");
    return "<default>";
}

} // namespace softg::font
