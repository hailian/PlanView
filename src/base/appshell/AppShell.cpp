#include "base/appshell/AppShell.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "base/appshell/FontSetup.h"
#include "base/appshell/Theme.h"
#include "base/log/Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// ImGui Win32 后端的消息处理入口（后端内部导出）
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace softg {

static const wchar_t* kWndClassName = L"SoftGAppShell";

LRESULT WINAPI AppShell::wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 1;

    AppShell* self = reinterpret_cast<AppShell*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (self && wParam != SIZE_MINIMIZED)
            self->onResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DPICHANGED:  // 跨不同缩放比例的显示器拖动时，系统通知新 DPI
        if (self)
            self->onDpiChanged((float)HIWORD(wParam) / 96.0f, lParam);
        return 0;
    case WM_SYSCOMMAND: // 屏蔽 Alt 应用菜单键，避免抢占 ImGui 快捷键
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool AppShell::createWindow(const AppConfig& config) {
    // Per-Monitor DPI：保证画面与文字在不同缩放比例显示器上清晰
    if (::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == NULL) {
        SOFTG_LOG_WARN("SetProcessDpiAwarenessContext 失败，回退系统默认 DPI 行为");
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWndClassName;
    if (!::RegisterClassExW(&wc)) {
        SOFTG_LOG_ERROR("RegisterClassExW 失败 (%lu)", GetLastError());
        return false;
    }

    // UTF-8 标题 -> UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, config.windowTitle.c_str(), -1, nullptr, 0);
    std::wstring wideTitle(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, config.windowTitle.c_str(), -1, wideTitle.data(), wlen);

    RECT rect = {0, 0, (LONG)config.windowSize.x, (LONG)config.windowSize.y};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = ::CreateWindowExW(0, kWndClassName, wideTitle.c_str(), WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        SOFTG_LOG_ERROR("CreateWindowExW 失败 (%lu)", GetLastError());
        return false;
    }
    // wndProc 通过 GWLP_USERDATA 取 self；必须在首次 WM_SIZE 前设置，否则 onResize 不生效
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);

    // ---- 系统 DPI 缩放 ----
    // 环境变量 SOFTG_UI_SCALE（如 1.5）可强制覆盖，便于调试与演示
    float scale = 0.0f;
    if (const char* env = std::getenv("SOFTG_UI_SCALE"))
        scale = (float)std::atof(env);
    if (scale <= 0.01f) {
        UINT dpi = ::GetDpiForWindow(hwnd_);
        scale = dpi ? (float)dpi / 96.0f : 1.0f;
    }
    dpiScale_ = std::clamp(scale, 0.5f, 4.0f);
    if (dpiScale_ != 1.0f) {
        // 默认窗口尺寸按缩放放大，保持物理尺寸一致；ShowWindow 前完成避免闪动
        RECT scaled = {0, 0, (LONG)(config.windowSize.x * dpiScale_ + 0.5f),
                           (LONG)(config.windowSize.y * dpiScale_ + 0.5f)};
        AdjustWindowRect(&scaled, WS_OVERLAPPEDWINDOW, FALSE);
        ::SetWindowPos(hwnd_, nullptr, 0, 0, scaled.right - scaled.left,
                       scaled.bottom - scaled.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ::ShowWindow(hwnd_, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd_);
    return true;
}

bool AppShell::createDeviceD3D() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    // createFlags |= D3D11_CREATE_DEVICE_DEBUG; // 需要调试层时打开
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &featureLevel, &context_);
    if (hr == DXGI_ERROR_UNSUPPORTED) { // 无硬件适配器时回退 WARP 软渲染
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags, nullptr, 0,
            D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &featureLevel, &context_);
    }
    if (FAILED(hr)) {
        SOFTG_LOG_ERROR("D3D11CreateDeviceAndSwapChain 失败 (hr=0x%08lX)", (unsigned long)hr);
        return false;
    }
    createRenderTarget();
    return true;
}

void AppShell::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
        backBuffer->Release();
    }
}

void AppShell::cleanupRenderTarget() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void AppShell::cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

void AppShell::onResize(UINT w, UINT h) {
    width_ = w;
    height_ = h;
    if (w == 0 || h == 0 || !swapChain_)
        return;
    cleanupRenderTarget();
    swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

void AppShell::onDpiChanged(float newScale, LPARAM lParam) {
    newScale = std::clamp(newScale, 0.5f, 4.0f);
    if (std::fabs(newScale - dpiScale_) < 0.01f)
        return;
    float factor = newScale / dpiScale_;
    dpiScale_ = newScale;
    if (ImGui::GetCurrentContext()) { // 上下文未建时仅记录，run() 初始化时统一应用
        ImGui::GetStyle().ScaleAllSizes(factor);
        ImGui::GetStyle().FontScaleMain = newScale;
    }
    SOFTG_LOG_INFO("显示器缩放变更: %.0f%%", newScale * 100.0f);
    // 按 OS 建议矩形调整窗口，保持物理尺寸观感
    const RECT* suggested = (const RECT*)lParam;
    ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

int AppShell::run(const AppConfig& config, const std::function<bool(AppShell&)>& frame) {
    if (!createWindow(config))
        return 1;
    if (!createDeviceD3D())
        return 1;

    // ---- ImGui 初始化 ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    static std::string iniPath = config.iniFilename; // 生命周期覆盖整个循环
    io.IniFilename = iniPath.c_str();

    theme::applyModern(); // 现代深色主题（替代 ImGui 默认样式）
    // 系统缩放：尺寸度量与字号按 DPI 一次缩放（1.92 动态字体系统按需光栅化，无需重载字体）
    if (dpiScale_ != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(dpiScale_);
        ImGui::GetStyle().FontScaleMain = dpiScale_;
    }
    SOFTG_LOG_INFO("UI 缩放: %.0f%%", dpiScale_ * 100.0f);
    font::setupChineseFont(18.0f);

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, context_);

    bool running = true;
    int exitCode = 0;
    MSG msg = {};
    while (running) {
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
                exitCode = (int)msg.wParam;
            }
        }
        if (!running)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (!frame(*this)) {
            running = false;
            ::DestroyWindow(hwnd_);
        }

        ImGui::Render();
        const float clearColor[4] = {0.055f, 0.067f, 0.086f, 1.0f}; // 与主题 kDarkest 一致
        context_->OMSetRenderTargets(1, &rtv_, nullptr);
        context_->ClearRenderTargetView(rtv_, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = swapChain_->Present(1, 0); // vsync
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            SOFTG_LOG_ERROR("D3D 设备丢失 (hr=0x%08lX)，退出", (unsigned long)hr);
            running = false;
            exitCode = 1;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    ::UnregisterClassW(kWndClassName, GetModuleHandleW(nullptr));
    return exitCode;
}

AppShell::~AppShell() = default;

} // namespace softg
