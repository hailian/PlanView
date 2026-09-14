// PollWorker — 后台轮询线程（唯一网络 I/O 所在地）。
// 契约（不可违反）：worker 绝不触碰 UI 侧的 Project/RuntimeEngine；
// 一切数据经互斥量保护的 POD 队列跨线程（读结果出 / 写请求入）。
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/data/IDataSource.h"
#include "base/data/frame/FrameDataSource.h"
#include "base/model/Project.h"
#include "base/packet/SerialLink.h"

namespace pv::viewer {

class PollWorker {
public:
    ~PollWorker() { stop(); }

    // 启动（工程设置 + tag 定义做 worker 私有快照；UI 侧后续编辑不影响运行）
    // 帧数据源启用时创建 FrameDataSource，否则使用 PlanView TCP 行协议数据源
    void start(const ProjectSettings& settings, const std::vector<Tag>& tags);
    void stop();
    bool running() const { return thread_.joinable(); }

    // 数据源运行开关（帧数据源手动启停；初始值 = autoStart，行协议恒为开）：
    // 关闭时断开连接、标签报「数据源未启动」；打开后自动连接
    void setSourceRunning(bool on) { sourceRunning_.store(on); }
    bool sourceRunning() const { return sourceRunning_.load(); }

    // UI 线程接口
    void enqueueWrite(const TagName& tag, TagValue value);
    std::vector<TagReadResult> drainResults();
    bool isConnected() const { return connected_.load(); }
    std::string lastError();

    // 报文监视：取走帧数据源最近收到的原始帧（仅帧数据源产生）
    void drainFrames(std::deque<FrameDataSource::FrameLogEntry>& out);
    bool isFrameSource() const { return frameSource_ != nullptr; }

    // 通信统计（UI 每帧拉取，画布卡片显示；帧数据源专用）
    std::string lastFrameTimeText();     // 最后一帧接收时间（空 = 未收到）
    uint64_t matchedFrameCount();        // 符合协议的帧计数（总）
    // 按拆帧配置索引分组的计数（协议组：协议卡只显示自己帧头的命中数）
    std::map<int, uint64_t> matchedFrameCountByIndex();

private:
    void run();
    void pushResultLocked(std::vector<TagReadResult>&& results);

    // 数据目的转发链路：一条 sink = 一种传输的一条连接（懒连接、断线退避重试）
    struct SinkLink {
        FrameSinkSettings cfg;
        std::unique_ptr<packet::TcpLink> tcp;
        std::unique_ptr<packet::UdpLink> udp;
        std::unique_ptr<packet::SerialLink> serial;
        bool up = false;          // 链路已建立
        std::chrono::steady_clock::time_point nextTry{}; // 断线后的下次重试时刻
    };
    void connectSink(SinkLink& sk, std::string& err);
    void forwardFrames(const std::deque<FrameDataSource::FrameLogEntry>& frames);

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> sourceRunning_{true}; // 数据源是否运行（手动启停 / autoStart）

    std::mutex m_;
    std::deque<TagReadResult> results_;
    std::deque<std::pair<TagName, TagValue>> writes_;
    std::deque<FrameDataSource::FrameLogEntry> frameLog_;
    std::string lastError_;

    ProjectSettings settings_;
    std::vector<Tag> tags_;  // worker 私有快照
    FrameDataSource* frameSource_ = nullptr;  // 由 run() 持有的源转换而来
    std::vector<SinkLink> sinks_;             // 由 run() 创建（sourceName 匹配生效数据源）
    std::atomic<uint64_t> matchedFrames_{0};  // 统计快照（run 循环刷新）
    std::mutex statM_;
    std::string lastFrameTime_;
    std::map<int, uint64_t> matchedByIdx_;
};

} // namespace pv::viewer
