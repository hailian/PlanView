// PollWorker — 后台轮询线程（唯一网络 I/O 所在地）。
// 契约（不可违反）：worker 绝不触碰 UI 侧的 Project/RuntimeEngine；
// 一切数据经互斥量保护的 POD 队列跨线程（读结果出 / 写请求入）。
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/data/IDataSource.h"
#include "base/data/frame/FrameDataSource.h"
#include "base/model/Project.h"

namespace softg::viewer {

class PollWorker {
public:
    ~PollWorker() { stop(); }

    // 启动（工程设置 + tag 定义做 worker 私有快照；UI 侧后续编辑不影响运行）
    // 帧数据源启用时创建 FrameDataSource，否则使用 SoftG TCP 行协议数据源
    void start(const ProjectSettings& settings, const std::vector<Tag>& tags);
    void stop();
    bool running() const { return thread_.joinable(); }

    // UI 线程接口
    void enqueueWrite(const TagName& tag, TagValue value);
    std::vector<TagReadResult> drainResults();
    bool isConnected() const { return connected_.load(); }
    std::string lastError();

    // 报文监视：取走帧数据源最近收到的原始帧（仅帧数据源产生）
    void drainFrames(std::deque<FrameDataSource::FrameLogEntry>& out);
    bool isFrameSource() const { return frameSource_ != nullptr; }

private:
    void run();
    void pushResultLocked(std::vector<TagReadResult>&& results);

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> connected_{false};

    std::mutex m_;
    std::deque<TagReadResult> results_;
    std::deque<std::pair<TagName, TagValue>> writes_;
    std::deque<FrameDataSource::FrameLogEntry> frameLog_;
    std::string lastError_;

    ProjectSettings settings_;
    std::vector<Tag> tags_;  // worker 私有快照
    FrameDataSource* frameSource_ = nullptr;  // 由 run() 持有的源转换而来
};

} // namespace softg::viewer
