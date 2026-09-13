#include "viewer/PollWorker.h"

#include "base/data/DataSourceManager.h"
#include "base/log/Log.h"

#include <chrono>

namespace softg::viewer {

void PollWorker::start(const ProjectSettings& settings, const std::vector<Tag>& tags) {
    stop();
    settings_ = settings;
    tags_ = tags;
    stopFlag_ = false;
    connected_ = false;
    thread_ = std::thread([this] { run(); });
}

void PollWorker::stop() {
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    connected_ = false;
}

void PollWorker::enqueueWrite(const TagName& tag, TagValue value) {
    std::lock_guard<std::mutex> g(m_);
    writes_.emplace_back(tag, std::move(value));
}

std::vector<TagReadResult> PollWorker::drainResults() {
    std::lock_guard<std::mutex> g(m_);
    std::vector<TagReadResult> out(std::make_move_iterator(results_.begin()),
                                   std::make_move_iterator(results_.end()));
    results_.clear();
    return out;
}

std::string PollWorker::lastError() {
    std::lock_guard<std::mutex> g(m_);
    return lastError_;
}

void PollWorker::drainFrames(std::deque<FrameDataSource::FrameLogEntry>& out) {
    std::lock_guard<std::mutex> g(m_);
    while (!frameLog_.empty()) {
        out.push_back(std::move(frameLog_.front()));
        frameLog_.pop_front();
    }
}

void PollWorker::pushResultLocked(std::vector<TagReadResult>&& results) {
    std::lock_guard<std::mutex> g(m_);
    // 上限保护：UI 卡顿时丢最旧的（每轮全量刷新，丢帧无害）
    for (auto& r : results) {
        results_.push_back(std::move(r));
        if (results_.size() > 64) results_.pop_front();
    }
}

void PollWorker::run() {
    DataSourceManager mgr;
    // 数据源组件选择：帧数据源（TCP/UDP/串口 + 自配置规约）或 SoftG 行协议
    std::unique_ptr<IDataSource> ds;
    if (settings_.frame.enabled) {
        ds = mgr.createFrame(settings_.frame);
        frameSource_ = static_cast<FrameDataSource*>(ds.get());
    } else {
        ds = mgr.createTcp(settings_.tcp);
        frameSource_ = nullptr;
    }

    // worker 私有标签指针表（快照，不触碰 UI 侧对象）
    std::vector<const Tag*> tagPtrs;
    tagPtrs.reserve(tags_.size());
    for (const auto& t : tags_) tagPtrs.push_back(&t);

    auto failAll = [&](const char* why) {
        std::vector<TagReadResult> results(tagPtrs.size());
        for (size_t i = 0; i < tagPtrs.size(); ++i) {
            results[i].tag = tagPtrs[i]->name;
            results[i].ok = false;
            results[i].quality = TagQuality::CommLost;
            results[i].error = why;
        }
        pushResultLocked(std::move(results));
        std::lock_guard<std::mutex> g(m_);
        lastError_ = why;
    };

    while (!stopFlag_) {
        if (!ds->isConnected()) {
            std::string err;
            if (!ds->connect(err)) {
                failAll(err.c_str());
                connected_ = false;
                // 退避重连
                for (int i = 0; i < 10 && !stopFlag_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            connected_ = true;
        }

        // 1) 处理写队列（帧数据源只收不发：丢弃写请求，不影响连接状态）
        std::vector<std::pair<TagName, TagValue>> writes;
        {
            std::lock_guard<std::mutex> g(m_);
            writes.assign(std::make_move_iterator(writes_.begin()),
                          std::make_move_iterator(writes_.end()));
            writes_.clear();
        }
        if (!ds->supportsWrite()) {
            if (!writes.empty())
                SOFTG_LOG_WARN("帧数据源不支持写回，丢弃 %d 条写请求", (int)writes.size());
        } else {
            bool writeFailed = false;
            for (auto& [name, value] : writes) {
                const Tag* t = nullptr;
                for (const auto& tag : tags_)
                    if (tag.name == name) t = &tag;
                if (!t) continue;
                std::string err;
                if (!ds->writeTag(*t, value, err)) {
                    SOFTG_LOG_WARN("写标签 %s 失败: %s", name.c_str(), err.c_str());
                    writeFailed = true;
                    std::lock_guard<std::mutex> g(m_);
                    lastError_ = err;
                }
            }
            if (writeFailed) {
                ds->disconnect();
                connected_ = false;
                continue;
            }
        }

        // 1.5) 帧数据源：转发原始报文给 UI 监视
        if (frameSource_) {
            std::deque<FrameDataSource::FrameLogEntry> frames;
            frameSource_->drainFrameLog(frames);
            if (!frames.empty()) {
                std::lock_guard<std::mutex> g(m_);
                for (auto& f : frames) {
                    frameLog_.push_back(std::move(f));
                    if (frameLog_.size() > 200) frameLog_.pop_front();
                }
            }
        }

        // 2) 轮询读取
        if (!tagPtrs.empty()) {
            auto results = ds->readTags(tagPtrs);
            bool anyCommLost = false;
            for (const auto& r : results)
                if (r.quality == TagQuality::CommLost) anyCommLost = true;
            pushResultLocked(std::move(results));
            if (anyCommLost) {
                ds->disconnect();  // transact 已断开；下轮重连
                connected_ = false;
                continue;
            }
        }

        // 3) 间隔（可中断 sleep）
        for (int slept = 0; slept < settings_.tcp.pollMs && !stopFlag_; slept += 20)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ds->disconnect();
    frameSource_ = nullptr;
}

} // namespace softg::viewer
