// UsbLink — USB 报文链路：libusb 打开 WinUSB/libusbK 设备（后台线程收 IN 端点字节）
// + OUT 端点发送原始字节。与串口同为字节流（bulk 传输不保报文边界），拆帧由
// FrameSplitter 在消费侧完成，本类只搬运原始字节。数据块复用 TcpChunk。
// libusb-1.0.dll 为运行时可选依赖：未放置时 start 返回 false + 放置提示 err。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/packet/TcpLink.h" // TcpChunk
#include "base/packet/UsbApi.h"

namespace pv::packet {

// 设备选择 token："vid:pid[:serial]"（vid/pid 为 1..4 位十六进制；serial 区分同
// 型号多台设备，无序列号设备省略第三段）。作为工程持久化的设备标识
bool parseDeviceToken(const std::string& token, uint16_t& vid, uint16_t& pid,
                      std::string& serial, std::string& err);
std::string makeDeviceToken(uint16_t vid, uint16_t pid, const std::string& serial);

// 端点号十六进制文本（"81"/"01"，容忍 "0x" 前缀）→ 数值；空串/非法返回 false
bool parseEndpointHex(const std::string& text, uint8_t& ep, std::string& err);

class UsbLink {
public:
    UsbLink() = default;
    ~UsbLink() { stop(); }
    UsbLink(const UsbLink&) = delete;
    UsbLink& operator=(const UsbLink&) = delete;

    // 打开设备并按需启动收包线程。device："vid:pid[:serial]"；
    // interfaceNum：bInterfaceNumber（0 起）；epIn/epOut：端点 hex 文本，空 = 自动
    // 选择该接口第一个批量端点（无批量则中断端点）。requireIn/requireOut：
    // 数据源 = true/false（收字节流）；数据目的 = false/true（发帧）。
    // 配置校验先于驱动检查（无 libusb 的机器上坏配置仍得到确定性错误）。失败 false + err
    bool start(const std::string& device, int interfaceNum, const std::string& epIn,
               const std::string& epOut, bool requireIn, bool requireOut, std::string& err);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // OUT 端点发送原始字节（数据目的转发 / 调试下发）
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的字节块（消费线程调用，喂给 FrameSplitter）
    void drain(std::deque<TcpChunk>& out);

    // 最近一次 IO 错误（拔线/设备移除时置位）
    std::string lastError();

private:
    void recvLoop(); // 在 IN 端点上周期性传输直到停止/出错

    const libusb::Api* api_ = nullptr; // 常驻单例（进程不卸载）
    void* handle_ = nullptr;           // libusb_device_handle*
    uint8_t epIn_ = 0, epOut_ = 0;     // 0 = 该方向无端点（未启用）
    bool epInInterrupt_ = false, epOutInterrupt_ = false;
    int iface_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<TcpChunk> inbox_;
    std::string lastError_;
};

} // namespace pv::packet
