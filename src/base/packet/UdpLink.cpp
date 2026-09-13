#include "base/packet/UdpLink.h"

#include "base/log/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace softg::packet {

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

bool UdpLink::start(int localPort, std::string& err) {
    stop();

    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup 失败";
        return false;
    }

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        err = "socket 创建失败";
        ::WSACleanup();
        return false;
    }
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
    DWORD tv = 300; // 收包轮询超时，便于线程检查退出标志
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    sock_ = (uintptr_t)s;
    running_ = true;
    thread_ = std::thread([this] { recvLoop(); });
    SOFTG_LOG_INFO("UDP 监听启动: 端口 %d", localPort);
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
    SOFTG_LOG_INFO("UDP 监听停止");
}

void UdpLink::setRemote(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteHost_ = host;
    remotePort_ = port;
}

bool UdpLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (sock_ == (uintptr_t)-1) {
        err = "UDP 未启动，请先绑定本地端口";
        return false;
    }
    sockaddr_in addr{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fillAddr(remoteHost_, remotePort_, addr, err)) return false;
    }
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
    while (running_) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = ::recvfrom((SOCKET)sock_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT)
                continue;
            break; // socket 已关闭或其他错误
        }
        char ip[64];
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        UdpPacket pkt;
        pkt.from = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
        pkt.data.assign(buf, buf + n);
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.push_back(std::move(pkt));
    }
}

} // namespace softg::packet
