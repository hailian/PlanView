#include "base/packet/UsbApi.h"

#include <windows.h>

#include <mutex>

namespace pv::packet::libusb {

namespace {

constexpr int kStrBuf = 256; // libusb 文档建议的 ASCII 字符串缓冲长度

// 解析全部函数指针；任一缺失视为 DLL 不可用
bool resolve(HMODULE m, Api& api) {
    auto req = [&](const char* name, void** fn) {
        *fn = (void*)::GetProcAddress(m, name);
        return *fn != nullptr;
    };
    if (!req("libusb_init", (void**)&api.init)) return false;
    if (!req("libusb_get_device_list", (void**)&api.get_device_list)) return false;
    if (!req("libusb_free_device_list", (void**)&api.free_device_list)) return false;
    if (!req("libusb_get_device_descriptor", (void**)&api.get_device_descriptor)) return false;
    if (!req("libusb_open", (void**)&api.open)) return false;
    if (!req("libusb_close", (void**)&api.close)) return false;
    if (!req("libusb_get_string_descriptor_ascii", (void**)&api.get_string_ascii)) return false;
    if (!req("libusb_get_active_config_descriptor", (void**)&api.get_active_config))
        return false;
    if (!req("libusb_free_config_descriptor", (void**)&api.free_config)) return false;
    if (!req("libusb_claim_interface", (void**)&api.claim_interface)) return false;
    if (!req("libusb_release_interface", (void**)&api.release_interface)) return false;
    if (!req("libusb_bulk_transfer", (void**)&api.bulk_transfer)) return false;
    if (!req("libusb_interrupt_transfer", (void**)&api.interrupt_transfer)) return false;
    if (!req("libusb_error_name", (void**)&api.error_name)) return false;
    api.module = m;
    return true;
}

// ---- 进程内单例（成功/失败均缓存）----
std::once_flag g_once;
Api g_api;
bool g_ok = false;
std::string g_err;

void probe() {
    // libusb 无标准安装路径：默认搜索（程序目录 + System32）恰好覆盖
    // 「DLL 放 exe 旁」与「放 System32」两种部署方式，无需额外回退目录
    HMODULE m = ::LoadLibraryExW(L"libusb-1.0.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        g_err = "未检测到 libusb-1.0.dll（USB 传输需将其放到程序目录或 System32；"
                "设备需 WinUSB/libusb 驱动，可用 Zadig 安装）";
        return;
    }
    if (!resolve(m, g_api)) {
        g_err = "libusb-1.0.dll 缺少所需接口（请更换较新版本）";
        ::FreeLibrary(m);
        return;
    }
    // 默认上下文初始化一次并常驻（与 Npcap 同策略：进程生命周期内不卸载）
    if (g_api.init(nullptr) != 0) {
        g_err = "libusb 初始化失败";
        ::FreeLibrary(m);
        return;
    }
    g_ok = true;
}

// 尽力读字符串描述符（读不到返回空——不影响枚举/匹配）
std::string readString(const Api* api, Handle* h, uint8_t index) {
    if (!index) return {};
    unsigned char buf[kStrBuf];
    int n = api->get_string_ascii(h, index, buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = 0;
    return (const char*)buf;
}

} // namespace

const Api* instance(std::string& err) {
    std::call_once(g_once, probe);
    if (!g_ok) err = g_err;
    return g_ok ? &g_api : nullptr;
}

std::vector<DeviceInfo> listDevices(std::string& err) {
    std::vector<DeviceInfo> out;
    const Api* api = instance(err);
    if (!api) return out;

    Device** list = nullptr;
    int n = (int)api->get_device_list(nullptr, &list);
    if (n < 0 || !list) {
        err = std::string("枚举 USB 设备失败: ") +
              (api->error_name ? api->error_name(n) : "?");
        return out;
    }
    for (int i = 0; i < n; ++i) {
        DeviceDescriptor d{};
        if (api->get_device_descriptor(list[i], &d) != 0) continue;
        // 打不开（驱动不兼容/被独占）即用不了：不列出，下拉只剩真正可选的设备
        Handle* h = nullptr;
        if (api->open(list[i], &h) != 0) continue;
        DeviceInfo info;
        info.vid = d.idVendor;
        info.pid = d.idProduct;
        info.serial = readString(api, h, d.iSerialNumber);
        info.product = readString(api, h, d.iProduct);
        api->close(h);
        out.push_back(std::move(info));
    }
    api->free_device_list(list, 1);
    return out;
}

} // namespace pv::packet::libusb
