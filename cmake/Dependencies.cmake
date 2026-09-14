# 第三方依赖：全部 FetchContent + 精确 pin 的 tag。
# 本机网络对 github 不稳定，默认使用 gitee 镜像（tag 与上游 commit 一致）；
# 网络环境变化时可用 -D PV_IMGUI_REPO=... / -D PV_JSON_REPO=... 覆盖。
# 升级依赖 = 修改对应 GIT_TAG，是显式动作。

include(FetchContent)

# 依赖缓存放在 out/deps：清掉 out/build 不需要重新下载
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/out/deps" CACHE PATH "FetchContent 缓存目录")

set(PV_IMGUI_REPO "https://gitee.com/zzzzhan3_pri/imgui.git" CACHE STRING "imgui 仓库")
set(PV_JSON_REPO  "https://gitee.com/mirrors/nlohmann-json.git" CACHE STRING "nlohmann/json 仓库")

# ---- Dear ImGui (docking 分支) ----
# imgui 无 CMake 支持，手工组装 static 库：核心 4 个 cpp + stdlib 辅助 + Win32/DX11 后端
set(IMGUI_TAG "v1.92.9b-docking" CACHE STRING "imgui docking tag")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY ${PV_IMGUI_REPO}
    GIT_TAG ${IMGUI_TAG}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
)
target_link_libraries(imgui PUBLIC d3d11 dxgi dwmapi user32 gdi32 imm32)

# ---- nlohmann/json ----
set(JSON_BuildTests OFF CACHE BOOL "")
set(JSON_Install OFF CACHE BOOL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY ${PV_JSON_REPO}
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
