// FrameDataSource — 帧数据源组件：TCP/UDP/串口接入 + 自配置规约解析 + 标签槽位映射。
// TCP/串口字节流按 TLV / 帧头+Length 拆帧；UDP 数据报天然成帧。
// 传输角色：客户端 connect 远端 / 服务端监听（TCP listen / UDP bind）本地端口；串口无角色。
// 推收结合：收包线程持续成帧并解析缓存；readTags 返回各标签槽位的最新解析值。
// v1 只收不发（supportsWrite=false），写回走原 SoftG 行协议数据源。
#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "base/data/IDataSource.h"
#include "base/data/frame/FrameSourceSettings.h"
#include "base/packet/SerialLink.h"
#include "base/packet/TcpLink.h"
#include "base/packet/UdpLink.h"

namespace softg {

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

    // 运行统计（画布卡片显示；线程安全）
    std::string lastFrameTimeText() const; // 最后一帧接收时间 HH:MM:SS（空 = 未收到）
    uint64_t matchedFrameCount() const;    // 符合协议的帧计数（成帧且命中至少一字段）

private:
    void pumpFrames();  // 收包队列 → 拆帧 → 规约解析 → 缓存 + 日志（worker 线程调用）

    FrameSourceSettings cfg_;
    packet::UdpLink udp_;
    packet::TcpLink tcp_;
    packet::SerialLink serial_; // 串口字节流：与 TCP 共用拆帧路径
    packet::FrameSplitter splitter_{cfg_.framing};

    mutable std::mutex mutex_; // 统计 getter 为 const 读
    std::map<int, TagValue> latestValue_;     // 标签槽位 → 最新解析值（数值/布尔/文本）
    std::deque<FrameLogEntry> frameLog_;
    std::string lastFrameTime_;               // 最后一帧接收时间（HH:MM:SS）
    uint64_t matchedFrames_ = 0;              // 符合协议的帧计数（成帧且命中至少一字段）
};

} // namespace softg
