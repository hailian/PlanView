#include "base/packet/TcpLink.h"

#include "base/log/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace softg::packet {

bool TcpLink::connect(const std::string& host, int port, std::string& err) {
    disconnect();

    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup 失败";
        return false;
    }

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket 创建失败";
        ::WSACleanup();
        return false;
    }

    // 非阻塞连接 + select 超时（与 TcpDataSource 同款）
    u_long nonBlock = 1;
    ::ioctlsocket(s, FIONBIO, &nonBlock);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        err = "IP 地址无效: " + host;
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    int rc = ::connect(s, (sockaddr*)&addr, sizeof(addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{0, 500 * 1000};
        rc = ::select(0, nullptr, &w, &e, &tv);
        if (rc <= 0 || FD_ISSET(s, &e)) {
            err = "连接超时或被拒绝: " + host + ":" + std::to_string(port);
            ::closesocket(s);
            ::WSACleanup();
            return false;
        }
    } else if (rc == SOCKET_ERROR) {
        err = "连接失败 (WSA=" + std::to_string(WSAGetLastError()) + "): " + host;
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    u_long block = 0;
    ::ioctlsocket(s, FIONBIO, &block);
    DWORD tv = 300; // 收包轮询超时，便于线程检查退出标志
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    sock_ = (uintptr_t)s;
    connected_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
    }
    thread_ = std::thread([this] { recvLoop(); });
    SOFTG_LOG_INFO("TCP 连接成功: %s:%d", host.c_str(), port);
    return true;
}

bool TcpLink::listen(int port, std::string& err) {
    disconnect();

    WSADATA wsa;
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup 失败";
        return false;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket 创建失败";
        ::WSACleanup();
        return false;
    }
    BOOL reuse = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "绑定端口失败: " + std::to_string(port) +
              " (WSA=" + std::to_string(WSAGetLastError()) + ")";
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    if (::listen(s, SOMAXCONN) == SOCKET_ERROR) {
        err = "listen 失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
        ::closesocket(s);
        ::WSACleanup();
        return false;
    }
    listenSock_ = (uintptr_t)s;
    listening_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
    }
    thread_ = std::thread([this] { acceptLoop(); });
    SOFTG_LOG_INFO("TCP 监听启动: 端口 %d", port);
    return true;
}

void TcpLink::disconnect() {
    if (!connected_ && !listening_ && sock_ == (uintptr_t)-1 && listenSock_ == (uintptr_t)-1)
        return;
    connected_ = false;
    listening_ = false;
    if (thread_.joinable()) thread_.join();
    if (sock_ != (uintptr_t)-1) {
        ::closesocket((SOCKET)sock_);
        sock_ = (uintptr_t)-1;
    }
    if (listenSock_ != (uintptr_t)-1) {
        ::closesocket((SOCKET)listenSock_);
        listenSock_ = (uintptr_t)-1;
    }
    ::WSACleanup();
}

bool TcpLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (!connected_) {
        err = "TCP 未连接";
        return false;
    }
    int n = ::send((SOCKET)sock_, (const char*)data.data(), (int)data.size(), 0);
    if (n == SOCKET_ERROR) {
        err = "发送失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    return true;
}

void TcpLink::drain(std::deque<TcpChunk>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

std::string TcpLink::lastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void TcpLink::recvLoop() {
    char buf[65535];
    while (connected_) {
        int n = ::recv((SOCKET)sock_, buf, sizeof(buf), 0);
        if (n > 0) {
            TcpChunk c;
            c.data.assign(buf, buf + n);
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(std::move(c));
        } else if (n == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "对端已关闭连接";
            break;
        } else {
            int wsa = WSAGetLastError();
            if (wsa == WSAEWOULDBLOCK || wsa == WSAETIMEDOUT)
                continue;
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "接收错误 (WSA=" + std::to_string(wsa) + ")";
            break;
        }
    }
    connected_ = false;
}

void TcpLink::acceptLoop() {
    while (listening_) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET((SOCKET)listenSock_, &r);
        timeval tv{0, 300 * 1000};
        int rc = ::select(0, &r, nullptr, nullptr, &tv);
        if (rc <= 0) continue; // 超时/出错：回到循环检查退出标志
        SOCKET c = ::accept((SOCKET)listenSock_, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD tvr = 300; // 收包轮询超时，便于线程检查退出标志
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tvr, sizeof(tvr));
        sock_ = (uintptr_t)c;
        connected_ = true;
        SOFTG_LOG_INFO("TCP 客户端已接入");
        recvLoop(); // 阻塞收字节，直到对端断开或出错
        ::closesocket(c);
        sock_ = (uintptr_t)-1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "对端已断开，等待重新接入";
        }
        SOFTG_LOG_INFO("TCP 客户端断开，回到监听");
    }
    connected_ = false;
}

} // namespace softg::packet
