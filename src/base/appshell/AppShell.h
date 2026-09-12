// AppShell — 两款 exe 共用的 Win32 + DX11 + Dear ImGui 引导。
// 负责窗口/设备/字体/主循环骨架；应用逻辑以每帧回调形式注入。
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <functional>
#include <string>

#include "imgui.h"

struct HWND__;
using HWND = HWND__*;

namespace softg {

struct AppConfig {
    std::string windowTitle = "SoftG App";
    ImVec2 windowSize = ImVec2(1600, 900);
    std::string iniFilename = "app.ini"; // ImGui 布局持久化文件名
};

class AppShell {
public:
    AppShell() = default;
    ~AppShell();
    AppShell(const AppShell&) = delete;
    AppShell& operator=(const AppShell&) = delete;

    // 运行主循环；frame 每帧调用一次，返回 false 退出。
    int run(const AppConfig& config, const std::function<bool(AppShell&)>& frame);

    HWND hwnd() const { return hwnd_; }
    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* deviceContext() const { return context_; }

    // 系统 DPI 缩放（96dpi = 1.0）。窗口创建时确定，跨屏拖动随 WM_DPICHANGED 更新。
    // 用于画布默认缩放等业务侧换算；ImGui 样式/字体的缩放由 AppShell 内部完成。
    float dpiScale() const { return dpiScale_; }

private:
    bool createWindow(const AppConfig& config);
    bool createDeviceD3D();
    void createRenderTarget();
    void cleanupRenderTarget();
    void cleanupDeviceD3D();
    void onResize(UINT w, UINT h);
    void onDpiChanged(float newScale, LPARAM lParam);

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    UINT width_ = 0, height_ = 0;
    float dpiScale_ = 1.0f;

    static LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

} // namespace softg
