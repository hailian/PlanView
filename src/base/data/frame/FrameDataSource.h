// FrameDataSource — 帧数据源组件：TCP/UDP/串口接入 + 自配置规约解析 + 标签槽位映射。
// TCP/串口字节流按 TLV / 帧头+Length 拆帧；UDP 数据报天然成帧。
// 传输角色：客户端 connect 远端 / 服务端监听（TCP listen / UDP bind）本地端口；串口无角色。
// 推收结合：收包线程持续成帧并解析缓存；readTags 返回各标签槽位的最新解析值。
// v1 只收不发（supportsWrite=false），写回走原 PlanView 行协议数据源。
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/data/IDataSource.h"
#include "base/data/frame/FrameSourceSettings.h"
#include "base/data/frame/TestFrameGen.h" // IncrementalFrameGen（自发送）
#include "base/packet/PcapLink.h"
#include "base/packet/SerialLink.h"
#include "base/packet/TcpLink.h"
#include "base/packet/UdpLink.h"

namespace pv {

class FrameDataSource : public IDataSource {
public:
    // 报文监视日志条目（收到的原始帧）
    struct FrameLogEntry {
        std::string timeText;             // HH:MM:SS
        std::vector<uint8_t> data;
    };

    explicit FrameDataSource(FrameSourceSettings settings);
    ~FrameDataSource() override;

    bool connect(std::string& err) override;   // 客户端(TCP/UDP): 连接远端；服务端(TCP/UDP): 监听/绑定；串口: 打开 COM 口
    void disconnect() override;
    bool isConnected() const override;

    // 返回各标签槽位最新解析值（工程换算按标签 scale/offset）；无数据 = Bad
    std::vector<TagReadResult> readTags(const std::vector<const Tag*>& tags) override;
    bool writeTag(const Tag& tag, TagValue value, std::string& err) override;
    bool supportsWrite() const override { return false; }

    // 报文监视：取走最近的原始帧（worker 线程调用后转发给 UI）
    void drainFrameLog(std::deque<FrameLogEntry>& out);

    // 数据目的转发：取走待转发的全部原始帧。与报文监视日志**解耦**——
    // 监视日志有 200 条 UI 上限，高帧率下会裁剪，转发功能不得因此丢帧
    //（队列仅由 worker 周期 drain，防御性上限 10 万条，超限丢最旧=尽力而为语义）
    void drainForwardFrames(std::deque<std::vector<uint8_t>>& out);

    // 运行统计（画布卡片显示；线程安全）
    std::string lastFrameTimeText() const; // 最后一帧接收时间 HH:MM:SS（空 = 未收到）
    uint64_t matchedFrameCount() const;    // 符合协议的帧计数（成帧且命中至少一字段）
    // 按拆帧配置索引分组的计数（协议组多帧头：各协议卡只显示自己帧头的命中数）
    std::map<int, uint64_t> matchedFrameCountByIndex() const;

private:
    void pumpFrames();  // 收包队列 → 拆帧 → 规约解析 → 缓存 + 日志（worker 线程调用）
    void startAutoSend(); // 自发送（模拟设备）：按拆帧配置分组建生成器并启动发送线程
    void sendLoop();      // 自发送线程：周期性生成帧并经当前链路发出

    FrameSourceSettings cfg_;
    packet::UdpLink udp_;
    packet::TcpLink tcp_;
    packet::SerialLink serial_; // 串口字节流：与 TCP 共用拆帧路径
    packet::PcapLink pcap_;     // 监听=镜像抓包：Npcap 混杂模式收第三方流量
    packet::FrameSplitter splitter_{cfg_.framing};
    packet::FlowSplitters flows_{cfg_.framing}; // 镜像 TCP 逐流拆帧（UDP 直通）
    // 协议组多帧头（含单配置回退）；命中索引供字段归属过滤
    std::vector<packet::FramingConfig> framings_;

    // 自发（本地模拟设备）：独立线程按周期产帧，直接喂进自身收包/解析管线
    //（无网络无端口；报文监视、数据目的转发与正常收包完全同路径）
    std::vector<IncrementalFrameGen> gens_; // 按拆帧配置分组的增量生成器
    std::thread sendThread_;
    std::atomic<bool> sending_{false};
    std::mutex genM_;                          // 自发生成帧 → pumpFrames 的交接队列
    std::deque<std::vector<uint8_t>> genInbox_;

    mutable std::mutex mutex_;                // 统计 getter 为 const 读
    // 解析值缓存按「地址组」存 vector（构造时按 cfg_.fields 的 address 去重分组），
    // 逐帧解析只写 vector 槽位，避免热路径逐字段做 map 运算；顺序写入天然
    // "后到者覆盖"，与旧版 latestValue_[address] 逐字段覆盖语义一致
    std::vector<int> groupAddr_;              // 地址组 → 标签槽位 address
    std::vector<int> groupOf_;                // 字段序号 → 地址组序号
    std::vector<TagValue> latestByGroup_;     // 地址组 → 最新解析值（数值/布尔/文本）
    std::vector<uint8_t> groupSet_;           // 地址组是否已有值（无数据 = Bad）
    std::deque<FrameLogEntry> frameLog_;      // 报文监视（上限 200，UI 观测用）
    std::deque<std::vector<uint8_t>> forward_;// 待转发原始帧（数据目的；与监视日志解耦）
    std::string lastFrameTime_;               // 最后一帧接收时间（HH:MM:SS）
    uint64_t matchedFrames_ = 0;              // 符合协议的帧计数（成帧且命中至少一字段）
    std::vector<uint64_t> matchedByIdx_;      // 同上，按 framingIndex 分组（协议组）
};

} // namespace pv
