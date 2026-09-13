// TcpLink — TCP 报文链路：连接远端收字节流（后台线程）+ 发送原始字节。
// 拆帧由 FrameSplitter 在消费侧完成，本类只搬运原始字节。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace softg::packet {

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

    // 连接远端（500ms 连接超时）；成功后启动收包线程
    bool connect(const std::string& host, int port, std::string& err);
    void disconnect();
    bool isConnected() const { return connected_; }

    // 发送原始字节（报文调试中手写 HEX）
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的字节块（UI 线程每帧调用，喂给 FrameSplitter）
    void drain(std::deque<TcpChunk>& out);

    // 最近一次 IO/连接错误（连接被断开时置位）
    std::string lastError();

private:
    void recvLoop();

    uintptr_t sock_ = (uintptr_t)-1; // SOCKET
    std::atomic<bool> connected_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<TcpChunk> inbox_;
    std::string lastError_;
};

} // namespace softg::packet
