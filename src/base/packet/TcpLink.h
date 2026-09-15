// TcpLink — TCP 报文链路：连接远端（客户端）或监听本地（服务端）收字节流 +
// 发送原始字节。拆帧由 FrameSplitter 在消费侧完成，本类只搬运原始字节。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pv::packet {

// 一段收到的原始字节（可能不构成完整帧）
struct TcpChunk {
    std::vector<uint8_t> data;
};

class TcpLink {
public:
    TcpLink() = default;
    ~TcpLink() { disconnect(); }
    TcpLink(const TcpLink&) = delete;
    TcpLink& operator=(const TcpLink&) = delete;

    // 客户端：连接远端（500ms 连接超时）；成功后启动收包线程
    bool connect(const std::string& host, int port, std::string& err);
    // 服务端：监听本地端口，等待设备接入（单连接；断开后自动回到监听）。
    // 监听过滤（可选）：filterIp=="*" 收任意对端，否则只接受对端 IP 匹配的连接
    //（连接内双向字节流都进入解析——TCP 正反向命中）
    bool listen(int port, std::string& err, const std::string& filterIp = "*");
    void disconnect();
    // 监听中即视为已接入（避免消费方反复重连）
    bool isConnected() const { return connected_ || listening_; }
    // 服务端：监听中且尚无对端接入——发送无从投递，消费方应丢帧保监听而非拆链
    bool awaitingPeer() const { return listening_ && !connected_; }

    // 发送原始字节（报文调试中手写 HEX）
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的字节块（UI 线程每帧调用，喂给 FrameSplitter）
    void drain(std::deque<TcpChunk>& out);

    // 最近一次 IO/连接错误（连接被断开时置位）
    std::string lastError();

private:
    void recvLoop();   // 在 sock_ 上收字节直到断开（客户端/服务端接入后共用）
    void acceptLoop(); // 服务端：select + accept，接入后进入 recvLoop，断开回到监听

    uintptr_t sock_ = (uintptr_t)-1;       // SOCKET（当前连接）
    uintptr_t listenSock_ = (uintptr_t)-1; // SOCKET（监听，仅服务端）
    std::string filterIp_ = "*";            // 监听过滤：对端 IP（"*" 通配）
    std::atomic<bool> connected_{false};
    std::atomic<bool> listening_{false};   // 服务端监听中
    std::thread thread_;
    std::mutex mutex_;
    std::deque<TcpChunk> inbox_;
    std::string lastError_;
};

} // namespace pv::packet
