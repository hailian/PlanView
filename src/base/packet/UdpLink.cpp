#include "base/packet/UdpLink.h"

#include "base/log/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#ifndef SIO_UDP_CONNRESET
// MSWSock.h 中的定义；此处内联以免额外 SDK 头依赖。
// 连接式 UDP 下屏蔽「远端不可达 → WSAECONNRESET」导致的收包中断。
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#pragma comment(lib, "ws2_32.lib")

namespace pv::packet {

namespace {

bool fillAddr(const std::string& host, int port, sockaddr_in& addr, std::string& err) {
    addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        err = "IP 地址无效: " + host;
        return false;
    }
    return true;
}

} // namespace

// 建 socket 并完成 WSAStartup；失败由调用方 closesocket/WSACleanup
static SOCKET makeSocket(std::string& err) {
    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup 失败";
        return INVALID_SOCKET;
    }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        err = "socket 创建失败";
        ::WSACleanup();
    }
    // 加大收包缓冲：默认 ~64KB 只够千余个小报文，设备突发/高帧率下内核先丢
    //（尽力而为调大，失败不阻断）
    int rcvBuf = 1 << 20;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBuf, sizeof(rcvBuf));
    return s;
}

void UdpLink::launch(uintptr_t sock) {
    DWORD tv = 300; // 收包轮询超时，便于线程检查退出标志
    ::setsockopt((SOCKET)sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    sock_ = sock;
    running_ = true;
    thread_ = std::thread([this] { recvLoop(); });
}

bool UdpLink::start(int localPort, std::string& err, const std::string& filterIp,
                    int filterPort) {
    stop();
    filterIp_ = filterIp.empty() ? "*" : filterIp;
    filterPort_ = filterPort;

    SOCKET s = makeSocket(err);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)localPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "绑定端口失败: " + std::to_string(localPort) +
              " (WSA=" + std::to_string(WSAGetLastError()) + ")";
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    connected_ = false;
    launch((uintptr_t)s);
    PV_LOG_INFO("UDP 监听启动: 端口 %d", localPort);
    return true;
}

bool UdpLink::startClient(const std::string& host, int port, std::string& err) {
    stop();

    SOCKET s = makeSocket(err);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    if (!fillAddr(host, port, addr, err)) {
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    if (::connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "连接远端失败: " + host + ":" + std::to_string(port) +
              " (WSA=" + std::to_string(WSAGetLastError()) + ")";
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    // 关闭「远端不可达 → WSAECONNRESET」行为，否则对端未监听时收包线程会被打断
    BOOL reportConnReset = FALSE;
    DWORD bytesReturned = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &reportConnReset, sizeof(reportConnReset), nullptr, 0,
               &bytesReturned, nullptr, nullptr);
    setRemote(host, port); // 使 send() 可用
    connected_ = true;
    launch((uintptr_t)s);
    PV_LOG_INFO("UDP 客户端连接: %s:%d", host.c_str(), port);
    return true;
}

void UdpLink::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (sock_ != (uintptr_t)-1) {
        ::closesocket((SOCKET)sock_);
        sock_ = (uintptr_t)-1;
    }
    ::WSACleanup();
    PV_LOG_INFO("UDP 监听停止");
}

void UdpLink::setRemote(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteHost_ = host;
    remotePort_ = port;
    // 解析一次缓存：合法 IPv4 文本才有效（非法时 send 报错，不再逐包重试解析）
    in_addr a{};
    remoteAddr_ = ::inet_pton(AF_INET, host.c_str(), &a) == 1 ? a.s_addr : 0;
}

bool UdpLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (sock_ == (uintptr_t)-1) {
        err = "UDP 未启动，请先绑定本地端口";
        return false;
    }
    // 连接式 socket 走 send()：内核跳过逐包目的地址处理，回环小包高频发送可测出差距
    if (connected_) {
        int n = ::send((SOCKET)sock_, (const char*)data.data(), (int)data.size(), 0);
        if (n == SOCKET_ERROR) {
            err = "发送失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        return true;
    }
    uint32_t addrNet = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        addrNet = remoteAddr_;
    }
    if (addrNet == 0) {
        err = "UDP 远端地址无效: " + remoteHost_;
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)remotePort_);
    addr.sin_addr.s_addr = addrNet;
    int n = ::sendto((SOCKET)sock_, (const char*)data.data(), (int)data.size(), 0,
                     (sockaddr*)&addr, sizeof(addr));
    if (n == SOCKET_ERROR) {
        err = "发送失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    return true;
}

void UdpLink::drain(std::deque<UdpPacket>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

void UdpLink::recvLoop() {
    char buf[65535];
    // 对端字符串缓存：同一设备连发时免逐包 inet_ntop + 组串（收包热路径）。
    // 仅本线程读写，无锁
    sockaddr_in lastFrom{};
    char lastIp[64] = {};
    std::string lastFromText;
    while (running_) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = ::recvfrom((SOCKET)sock_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT)
                continue;
            break; // socket 已关闭或其他错误
        }
        if (::memcmp(&from, &lastFrom, sizeof(from)) != 0) {
            ::inet_ntop(AF_INET, &from.sin_addr, lastIp, sizeof(lastIp));
            lastFromText = std::string(lastIp) + ":" + std::to_string(ntohs(from.sin_port));
            lastFrom = from;
        }
        // 监听过滤：三元组 dip/dport 反向命中（源 == 过滤器）；未命中丢弃。
        // dip="*" 时不限 IP（正向场景：凡到达本端口的报文均命中）
        if ((filterIp_ != "*" && filterIp_ != lastIp) ||
            (filterPort_ != 0 && (int)ntohs(from.sin_port) != filterPort_))
            continue;
        UdpPacket pkt;
        pkt.from = lastFromText;
        pkt.data.assign(buf, buf + n);
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.push_back(std::move(pkt));
    }
}

} // namespace pv::packet
