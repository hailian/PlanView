// UdpLink — UDP 报文链路：绑定本地端口收包（后台线程）+ 向远端发包。
// 供报文调试面板使用；一个实例 = 一个 UDP socket。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace softg::packet {

struct UdpPacket {
    std::string from;      // 对端 "ip:port"
    std::vector<uint8_t> data;
};

class UdpLink {
public:
    UdpLink() = default;
    ~UdpLink() { stop(); }
    UdpLink(const UdpLink&) = delete;
    UdpLink& operator=(const UdpLink&) = delete;

    // 绑定本地端口并启动收包线程；失败返回 false + err
    bool start(int localPort, std::string& err);
    void stop();
    bool isRunning() const { return running_; }

    void setRemote(const std::string& host, int port);
    // 向远端发送数据报；失败返回 false + err
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的数据报（UI 线程每帧调用）
    void drain(std::deque<UdpPacket>& out);

private:
    void recvLoop();

    uintptr_t sock_ = (uintptr_t)-1; // SOCKET
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<UdpPacket> inbox_;
    std::string remoteHost_ = "127.0.0.1";
    int remotePort_ = 0;
};

} // namespace softg::packet
