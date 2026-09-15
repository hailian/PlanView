#include "base/packet/VisaApi.h"

#include <windows.h>

#include <mutex>

namespace pv::packet::visa {

namespace {

// 解析全部函数指针；任一缺失视为不可用
bool resolve(HMODULE m, Api& api) {
    auto req = [&](const char* name, void** fn) {
        *fn = (void*)::GetProcAddress(m, name);
        return *fn != nullptr;
    };
    if (!req("viOpenDefaultRM", (void**)&api.openDefaultRM)) return false;
    if (!req("viOpen", (void**)&api.open)) return false;
    if (!req("viClose", (void**)&api.close)) return false;
    if (!req("viRead", (void**)&api.read)) return false;
    if (!req("viWrite", (void**)&api.write)) return false;
    if (!req("viSetAttribute", (void**)&api.setAttribute)) return false;
    if (!req("viStatusDesc", (void**)&api.statusDesc)) return false;
    if (!req("viFindRsrc", (void**)&api.findRsrc)) return false;
    if (!req("viFindNext", (void**)&api.findNext)) return false;
    api.module = m;
    return true;
}

// ---- 进程内单例（成功/失败均缓存）----
std::once_flag g_once;
Api g_api;
bool g_ok = false;
std::string g_err;

void probe() {
    // NI-VISA / Keysight IO Libraries / R&S VISA 均把 VISA 共享库装进 System32：
    // 默认搜索（程序目录 + System32）即覆盖。先 64 后 32——架构不符的 DLL
    // LoadLibrary 直接失败，无副作用（x64 进程加载 visa32.dll / 反之亦然）
    HMODULE m = ::LoadLibraryExW(L"visa64.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m)
        m = ::LoadLibraryExW(L"visa32.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) {
        g_err = "未检测到 VISA 运行时（VISA 传输需安装 NI-VISA 或 Keysight IO Libraries "
                "Suite 等，以访问 USBTMC/GPIB/以太网仪器）";
        return;
    }
    if (!resolve(m, g_api)) {
        g_err = "VISA 共享库缺少所需接口（请升级或更换 VISA 运行时）";
        ::FreeLibrary(m);
        return;
    }
    // 默认 RM 由各链路按需 viOpenDefaultRM（实现内部引用计数，代价低），此处不预建
    g_ok = true;
}

} // namespace

const Api* instance(std::string& err) {
    std::call_once(g_once, probe);
    if (!g_ok) err = g_err;
    return g_ok ? &g_api : nullptr;
}

std::string statusText(const Api* api, ViSession vi, ViStatus st) {
    if (!api || !api->statusDesc) return "?";
    char desc[kFindBufLen];
    if (api->statusDesc(vi, st, desc) < 0) return "?";
    return desc;
}

std::vector<std::string> listResources(std::string& err) {
    std::vector<std::string> out;
    const Api* api = instance(err);
    if (!api) return out;

    ViSession rm = 0;
    ViStatus st = api->openDefaultRM(&rm);
    if (st < 0) {
        err = "VISA 初始化失败: " + statusText(api, 0, st);
        return out;
    }
    ViSession findList = 0;
    ViUInt32 cnt = 0;
    char desc[kFindBufLen];
    st = api->findRsrc(rm, "?*INSTR", &findList, &cnt, desc);
    if (st < 0) {
        // 枚举失败不致命：仅下拉不可用，地址仍可手敲直连
        err = "VISA 资源枚举失败: " + statusText(api, rm, st);
        api->close(rm);
        return out;
    }
    if (cnt > 0) out.push_back(desc); // 首个资源已随 findRsrc 返回
    for (ViUInt32 i = 1; i < cnt; ++i) {
        char next[kFindBufLen];
        if (api->findNext(findList, next) < 0) break;
        out.push_back(next);
    }
    if (findList) api->close(findList);
    api->close(rm);
    return out;
}

} // namespace pv::packet::visa
