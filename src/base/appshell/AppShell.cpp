#include "base/appshell/AppShell.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "base/appshell/FontSetup.h"
#include "base/log/Log.h"

#include <windows.h>

#include <cstdio>
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

    ImGui::StyleColorsDark();
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
        const float clearColor[4] = {0.08f, 0.08f, 0.11f, 1.0f};
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
