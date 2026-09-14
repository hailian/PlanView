// SerialLink — 串口报文链路：Win32 CreateFile 打开 COM 口（后台线程收字节）+ 发送原始字节。
// 与 TCP 同为字节流，拆帧由 FrameSplitter 在消费侧完成，本类只搬运原始字节。
// 数据块复用 TcpChunk（同为「一段收到的原始字节」语义）。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/packet/TcpLink.h" // TcpChunk

namespace pv::packet {

class SerialLink {
public:
    SerialLink() = default;
    ~SerialLink() { close(); }
    SerialLink(const SerialLink&) = delete;
    SerialLink& operator=(const SerialLink&) = delete;

    // 打开串口并启动收包线程。port："COM3"（兼容只写数字 "3"）；
    // parity："无"/"奇"/"偶"；stopBits：1 或 2；失败返回 false + err
    bool open(const std::string& port, int baud, int dataBits, const std::string& parity,
              int stopBits, std::string& err);
    void close();
    bool isOpen() const { return open_.load(std::memory_order_acquire); }

    // 发送原始字节（当前未接协议下发，调试用途）
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的字节块（消费线程调用，喂给 FrameSplitter）
    void drain(std::deque<TcpChunk>& out);

    // 最近一次 IO 错误（拔线/设备移除时置位）
    std::string lastError();

private:
    void recvLoop(); // 在句柄上周期性 ReadFile 直到关闭/出错

    void* handle_ = nullptr; // HANDLE（void* 避免 proliferation windows.h）
    std::atomic<bool> open_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<TcpChunk> inbox_;
    std::string lastError_;
};

} // namespace pv::packet
