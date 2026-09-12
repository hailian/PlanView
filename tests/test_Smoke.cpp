// M0 冒烟测试：验证 softg_base + imgui 链接可用
#define SOFTG_TEST_MAIN
#include "SoftgTest.h"

#include "imgui.h"

TEST_CASE("ImGui 链接与上下文") {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    CHECK(io.Fonts != nullptr);
    ImGui::DestroyContext();
}

TEST_CASE("SoftgTest 自身") {
    int v = 1 + 1;
    CHECK(v == 2);
}
