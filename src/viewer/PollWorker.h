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
#include "base/model/Project.h"

namespace softg::viewer {

class PollWorker {
public:
    ~PollWorker() { stop(); }

    // 启动（tag 定义做 worker 私有快照；UI 侧后续编辑不影响运行）
    void start(const TcpSettings& settings, const std::vector<Tag>& tags);
    void stop();
    bool running() const { return thread_.joinable(); }

    // UI 线程接口
    void enqueueWrite(const TagName& tag, TagValue value);
    std::vector<TagReadResult> drainResults();
    bool isConnected() const { return connected_.load(); }
    std::string lastError();

private:
    void run();
    void pushResultLocked(std::vector<TagReadResult>&& results);

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> connected_{false};

    std::mutex m_;
    std::deque<TagReadResult> results_;
    std::deque<std::pair<TagName, TagValue>> writes_;
    std::string lastError_;

    TcpSettings settings_;
    std::vector<Tag> tags_;  // worker 私有快照
};

} // namespace softg::viewer
