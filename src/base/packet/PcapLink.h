// PcapLink — Npcap 抓包链路（混杂模式）：交换机镜像/SPAN 场景监听第三方流量。
// 三元组 dip/dport/协议 经 BPF 内核过滤（命中 = 任一方向），报文再由
// parseEthernetFrame 解出四元组与 L4 负载。UDP 数据报天然成帧；
// TCP 由 FlowSplitters 逐流拆帧（镜像流有重传/乱序，尽力而为不做重组）。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/packet/FrameCodec.h"
#include "base/packet/NpcapApi.h"

namespace pv::packet {

// 抓到并剥至 L4 负载的一个报文
struct CapPacket {
    std::string srcIp, dstIp;
    uint16_t srcPort = 0, dstPort = 0;
    bool tcp = false;
    std::vector<uint8_t> payload;
    std::string flowKey; // "srcIp:srcPort>dstIp:dstPort"（TCP 逐流拆帧的流标识）
};

// 以太帧 → CapPacket：跳过 802.1Q/802.1ad VLAN 标签栈后要求 IPv4 + TCP/UDP。
// 截断 / 非 IPv4 / 头部长度不合法返回 false（这类镜像包直接丢弃）。
bool parseEthernetFrame(const uint8_t* data, size_t len, CapPacket& out);

// 监听三元组 → BPF 过滤表达式。dip="*"（或空）只按 dport，命中 = 任一方向
//（正向=目的 dip:dport，反向=源 dip:dport，镜像场景收完整会话）。
// BPF 的 vlan 原语不穿透：带 VLAN 标签的镜像帧需显式并列放行。
std::string buildListenBpf(const std::string& dip, int dport, bool tcp);

// 逐流拆帧：UDP 数据报直通（一包一帧）；TCP 按流（四元组+方向）各建一个
// FrameSplitter，避免镜像的双向流互相掺杂。流数上限 32，超限整体清空重建
//（防扫描类场景撑爆内存；拆帧器随后按帧头重同步，短暂丢帧可接受）。
class FlowSplitters {
public:
    explicit FlowSplitters(const FramingConfig& config) : cfg_(config) {}

    void setConfig(const FramingConfig& config);
    void setConfigs(const std::vector<FramingConfig>& configs); // 协议组多帧头
    void feed(const CapPacket& pkt, std::vector<std::vector<uint8_t>>& out);
    void reset() { flows_.clear(); } // 连接重建时调用

private:
    FramingConfig cfg_;
    std::vector<FramingConfig> cfgs_; // 多帧头集合（空 = 单配置模式）
    std::map<std::string, FrameSplitter> flows_;
};

// Npcap 抓包链路：start 打开网卡（混杂模式）+ 安装 BPF 过滤器并启动抓包线程；
// 命中报文解析后进 inbox，由消费方 drain。未安装 Npcap 时 start 失败并给出
// 安装提示（连接级降级，与串口缺设备同路——交由 PollWorker 退避重试）。
class PcapLink {
public:
    PcapLink() = default;
    ~PcapLink() { stop(); }
    PcapLink(const PcapLink&) = delete;
    PcapLink& operator=(const PcapLink&) = delete;

    // device = Npcap 设备名（\Device\NPF\...）；bpf = 过滤表达式（空 = 全收）
    bool start(const std::string& device, const std::string& bpf, std::string& err);
    void stop();
    bool isRunning() const { return running_; }

    // 取走已抓到的报文（消费线程周期调用）
    void drain(std::deque<CapPacket>& out);

    std::string lastError();

private:
    static void onPacket(unsigned char* user, const npcap::PcapPkthdr* hdr,
                         const unsigned char* bytes);
    void capLoop();

    const npcap::Api* api_ = nullptr; // 进程内单例（常驻，指针稳定）
    void* handle_ = nullptr;          // pcap_t*
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::deque<CapPacket> inbox_;
    std::string lastError_;
};

} // namespace pv::packet
