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

namespace pv::packet {

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

    // 服务端：绑定本地端口并启动收包线程；失败返回 false + err。
    // 监听过滤（可选）：filterIp=="*" 且 filterPort==0 时收任意对端；
    // 否则只收源 ==（filterIp, filterPort）的数据报（监听数据源反向命中，
    // 未命中直接丢弃不进队列；filterPort==0 表示不限源端口）
    bool start(int localPort, std::string& err, const std::string& filterIp = "*",
               int filterPort = 0);
    // 客户端：connect 远端（仅收该对端，本地端口由系统临时分配）；失败返回 false + err
    bool startClient(const std::string& host, int port, std::string& err);
    void stop();
    bool isRunning() const { return running_; }

    void setRemote(const std::string& host, int port);
    // 向远端发送数据报；失败返回 false + err
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的数据报（UI 线程每帧调用）
    void drain(std::deque<UdpPacket>& out);

private:
    void recvLoop();
    // 公共收尾：设收包超时 + 启用收包线程（sock 已建立/绑定/连接）
    void launch(uintptr_t sock);

    uintptr_t sock_ = (uintptr_t)-1; // SOCKET
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<UdpPacket> inbox_;
    std::string remoteHost_ = "127.0.0.1";
    int remotePort_ = 0;
    std::string filterIp_ = "*"; // 监听过滤：源 IP（"*" 通配）
    int filterPort_ = 0;         // 监听过滤：源端口（0 不限）
};

} // namespace pv::packet
