// NpcapApi — wpcap.dll 运行时动态加载（libpcap C API 函数指针表）。
// 镜像抓包（交换机 SPAN）监听依赖 Npcap 驱动：编译期不依赖 Npcap SDK，
// 未安装时 load() 返回 false，上层给安装提示，其余功能不受影响。
// 进程内单例缓存：首次调用探测，结果（含失败）复用，不再反复 LoadLibrary。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pv::packet::npcap {

// pcap.h 关键类型的本仓镜像（不引 SDK 头；接口按 libpcap 稳定 ABI 声明）
using PcapT = void;

// struct pcap_if：设备链表结点（只遍历 name/description，地址族细节不关心）
struct PcapIf {
    PcapIf* next;
    char* name;
    char* description;
    void* addresses;
    unsigned int flags;
};

// struct bpf_program：编译后的过滤器程序
struct BpfProgram {
    unsigned int len;
    void* insns;
};

// struct pcap_pkthdr：时间戳 + 抓包长度（Windows timeval = 两个 32 位 long）
struct PcapPkthdr {
    long tv_sec;
    long tv_usec;
    unsigned int caplen;
    unsigned int len;
};

// pcap_dispatch 的回调签名
using PcapHandler = void (*)(unsigned char* user, const PcapPkthdr* hdr,
                             const unsigned char* bytes);

// libpcap C API 子集（按需声明）
struct Api {
    void* module = nullptr; // DLL 句柄（保持常驻）

    int (*findalldevs)(PcapIf** devices, char* errbuf) = nullptr;
    void (*freealldevs)(PcapIf* devices) = nullptr;
    PcapT* (*open_live)(const char* device, int snaplen, int promisc, int toMs,
                        char* errbuf) = nullptr;
    void (*close)(PcapT* handle) = nullptr;
    int (*compile)(PcapT* handle, BpfProgram* prog, const char* filter, int optimize,
                   unsigned int netmask) = nullptr;
    int (*setfilter)(PcapT* handle, BpfProgram* prog) = nullptr;
    void (*freecode)(BpfProgram* prog) = nullptr;
    int (*dispatch)(PcapT* handle, int count, PcapHandler handler,
                    unsigned char* user) = nullptr;
    char* (*geterr)(PcapT* handle) = nullptr;
    int (*datalink)(PcapT* handle) = nullptr;
    int (*sendpacket)(PcapT* handle, const unsigned char* buf, int size) = nullptr;
};

// 加载 wpcap.dll 并解析函数指针；失败返回 false + err（含安装提示）。
// 探测顺序：默认搜索路径（WinPcap 兼容模式装到 System32）→ Npcap 默认目录
// C:\Windows\System32\Npcap\（须 LOAD_WITH_ALTERED_SEARCH_PATH 使同目录
// 的 Packet.dll 依赖被正确解析）。
bool load(Api& out, std::string& err);

// 进程内单例：成功返回指针；失败返回 nullptr 并置 err（结果缓存）
const Api* instance(std::string& err);

// 枚举本机网卡：(NPF 设备名, 友好描述)。未安装 Npcap / 枚举失败返回空 + err。
// 设备名形如 \Device\NPF\{GUID}，跨重启稳定，作为工程持久化的网卡标识。
std::vector<std::pair<std::string, std::string>> listAdapters(std::string& err);

} // namespace pv::packet::npcap
