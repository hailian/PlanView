// VisaApi — VISA 共享库（visa64.dll / visa32.dll）运行时动态加载（VISA C API 函数指针表）。
// VISA 传输（数据源收帧 / 数据目的转发）经 NI-VISA / Keysight IO Libraries 等运行时统一
// 访问 USBTMC/GPIB/以太网仪器（viRead/viWrite API）：编译期不依赖 VISA SDK，未安装时
// instance() 返回 nullptr，上层给安装提示，其余功能不受影响。
// 进程内单例缓存：首次调用探测，结果（含失败）复用，不再反复 LoadLibrary。
#pragma once

#include <string>
#include <vector>

namespace pv::packet::visa {

// visa.h 关键类型的本仓镜像（不引 SDK 头；按 VISA 规范稳定 ABI 声明——
// Windows 上 unsigned long / long 恒为 32 位，x64 进程布局不变）
using ViUInt32 = unsigned long;
using ViInt32 = long;
using ViSession = ViUInt32; // 会话/对象句柄（RM、仪器、查找列表）
using ViStatus = ViInt32;   // 状态码：0=成功，正值=警告，负值=错误

// 状态码 / 属性常量（数值取自 VISA 规范 visa.h）
constexpr ViStatus kSuccess = 0;
constexpr ViStatus kErrorTmo = (ViStatus)0xBFFF0015ul; // VI_ERROR_TMO：读写超时
constexpr ViUInt32 kAttrTmoValue = 0x3FFF001Aul;       // VI_ATTR_TMO_VALUE（ms）
constexpr unsigned kFindBufLen = 256;                  // VI_FIND_BUFLEN：资源地址缓冲长度

// VISA C API 子集（按需声明；任一导出缺失视为 DLL 不可用）
struct Api {
    void* module = nullptr; // DLL 句柄（保持常驻）

    ViStatus (*openDefaultRM)(ViSession* sesn) = nullptr;
    ViStatus (*open)(ViSession sesn, const char* rsrcName, ViUInt32 mode, ViUInt32 lockTimeout,
                     ViSession* vi) = nullptr;
    ViStatus (*close)(ViSession vi) = nullptr;
    ViStatus (*read)(ViSession vi, unsigned char* buf, ViUInt32 cnt, ViUInt32* retCnt) = nullptr;
    ViStatus (*write)(ViSession vi, const unsigned char* buf, ViUInt32 cnt,
                      ViUInt32* retCnt) = nullptr;
    ViStatus (*setAttribute)(ViSession vi, ViUInt32 attr, ViUInt32 value) = nullptr;
    ViStatus (*statusDesc)(ViSession vi, ViStatus status, char desc[]) = nullptr;
    ViStatus (*findRsrc)(ViSession sesn, const char* expr, ViSession* findList, ViUInt32* retCnt,
                         char desc[]) = nullptr;
    ViStatus (*findNext)(ViSession findList, char desc[]) = nullptr;
};

// 进程内单例：成功返回指针；失败返回 nullptr 并置 err（含运行时安装提示）。结果缓存
const Api* instance(std::string& err);

// 错误码 → 文本（viStatusDesc；失败返回 "?"）
std::string statusText(const Api* api, ViSession vi, ViStatus st);

// 枚举本机仪器资源（viFindRsrc "?*INSTR"，覆盖 USB/GPIB/TCPIP 的 INSTR 类地址）。
// 未装 VISA 运行时 / 枚举失败返回空 + err；已装但无设备同样返回空（err 带原因）
std::vector<std::string> listResources(std::string& err);

} // namespace pv::packet::visa
