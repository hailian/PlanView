// VisaLink — VISA 报文链路：经 VISA 运行时打开仪器（viOpen；USBTMC/GPIB/以太网总线
// 统一），后台线程 viRead 收字节 + viWrite 发送原始字节。与串口同为字节流
//（USBTMC 虽保消息边界，TCPIP/串口类资源却是流），拆帧由 FrameSplitter 在
// 消费侧完成，本类只搬运原始字节。数据块复用 TcpChunk。
// VISA 运行时（visa64.dll/visa32.dll）为可选运行时依赖：未安装时 start 返回
// false + 安装提示 err。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/packet/TcpLink.h" // TcpChunk
#include "base/packet/VisaApi.h"

namespace pv::packet {

// VISA 资源地址合法性检查（非空且不超 VI_FIND_BUFLEN 长度）；失败置 err
bool validateVisaAddress(const std::string& address, std::string& err);

class VisaLink {
public:
    VisaLink() = default;
    ~VisaLink() { stop(); }
    VisaLink(const VisaLink&) = delete;
    VisaLink& operator=(const VisaLink&) = delete;

    // 打开仪器并按需启动收包线程。address：VISA 资源地址（如
    // "USB0::0x0957::0x1234::MY1::INSTR" / "TCPIP0::192.168.1.5::inst0::INSTR" /
    // "GPIB0::5::INSTR"）。requireRecv：数据源=true（收字节流）；
    // 数据目的=false（只发帧，读写超时相应放宽）。地址校验先于运行时检查
    //（无 VISA 的机器上坏地址仍得到确定性错误）。失败 false + err
    bool start(const std::string& address, bool requireRecv, std::string& err);
    void stop();
    bool isRunning() const { return running_.load(std::memory_order_acquire); }

    // viWrite 发送原始字节（数据目的转发 / SCPI 调试下发）
    bool send(const std::vector<uint8_t>& data, std::string& err);

    // 取走已收到的字节块（消费线程调用，喂给 FrameSplitter）
    void drain(std::deque<TcpChunk>& out);

    // 最近一次 IO 错误（拔线/设备移除时置位）
    std::string lastError();

private:
    void recvLoop(); // 在会话上周期性 viRead 直到停止/出错

    const visa::Api* api_ = nullptr; // 常驻单例（进程不卸载）
    visa::ViSession rm_ = 0;         // 默认 RM 会话（每链路一个，stop 时关闭）
    visa::ViSession vi_ = 0;         // 仪器会话
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<TcpChunk> inbox_;
    std::string lastError_;
};

} // namespace pv::packet
