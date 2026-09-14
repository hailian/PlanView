#include "base/packet/NpcapApi.h"

#include <windows.h>

#include <mutex>

namespace pv::packet::npcap {

namespace {

constexpr int kErrbufSize = 256; // PCAP_ERRBUF_SIZE

// 探测并解析全部函数指针；任一缺失视为 Npcap 不可用
bool resolve(HMODULE m, Api& api) {
    auto req = [&](const char* name, void** fn) {
        *fn = (void*)::GetProcAddress(m, name);
        return *fn != nullptr;
    };
    if (!req("pcap_findalldevs", (void**)&api.findalldevs)) return false;
    if (!req("pcap_freealldevs", (void**)&api.freealldevs)) return false;
    if (!req("pcap_open_live", (void**)&api.open_live)) return false;
    if (!req("pcap_close", (void**)&api.close)) return false;
    if (!req("pcap_compile", (void**)&api.compile)) return false;
    if (!req("pcap_setfilter", (void**)&api.setfilter)) return false;
    if (!req("pcap_freecode", (void**)&api.freecode)) return false;
    if (!req("pcap_dispatch", (void**)&api.dispatch)) return false;
    if (!req("pcap_geterr", (void**)&api.geterr)) return false;
    if (!req("pcap_datalink", (void**)&api.datalink)) return false;
    if (!req("pcap_sendpacket", (void**)&api.sendpacket)) return false;
    api.module = m;
    return true;
}

// ---- 进程内单例（成功/失败均缓存）----
std::once_flag g_once;
Api g_api;
bool g_ok = false;
std::string g_err;

void probe() {
    // 顺序 1：默认搜索路径（WinPcap API 兼容模式把 wpcap.dll/Packet.dll 装进 System32）
    HMODULE m = ::LoadLibraryExW(L"wpcap.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    // 顺序 2：Npcap 默认安装目录（非兼容模式时 DLL 不在搜索路径上）。
    // ALTERED_SEARCH_PATH：以 DLL 自身目录优先解析其 Packet.dll 依赖
    if (!m)
        m = ::LoadLibraryExW(L"C:\\Windows\\System32\\Npcap\\wpcap.dll", nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        g_err = "未检测到 Npcap 驱动（镜像抓包需安装 Npcap: www.npcap.com）";
        return;
    }
    if (!resolve(m, g_api)) {
        g_err = "wpcap.dll 缺少所需接口（请升级 Npcap 后重试）";
        ::FreeLibrary(m);
        return;
    }
    g_ok = true;
}

} // namespace

bool load(Api& out, std::string& err) {
    std::string ierr;
    const Api* a = instance(ierr);
    if (!a) {
        err = ierr;
        return false;
    }
    out = *a;
    return true;
}

const Api* instance(std::string& err) {
    std::call_once(g_once, probe);
    if (!g_ok) err = g_err;
    return g_ok ? &g_api : nullptr;
}

std::vector<std::pair<std::string, std::string>> listAdapters(std::string& err) {
    std::vector<std::pair<std::string, std::string>> out;
    const Api* api = instance(err);
    if (!api) return out;

    char ebuf[kErrbufSize];
    PcapIf* devices = nullptr;
    if (api->findalldevs(&devices, ebuf) < 0) {
        err = std::string("枚举网卡失败: ") + ebuf;
        return out;
    }
    for (PcapIf* it = devices; it; it = it->next) {
        if (!it->name) continue;
        // 描述可能为空（回退显示设备名）；跳过 Npcap 虚拟空设备
        std::string desc = it->description ? it->description : "";
        out.emplace_back(it->name, desc);
    }
    api->freealldevs(devices);
    return out;
}

} // namespace pv::packet::npcap
