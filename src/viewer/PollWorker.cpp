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
    // 帧数据源按 autoStart 决定初始状态（默认关 = 打开工程不主动连接，等手动启动）；
    // 行协议无此开关，维持打开即连
    sourceRunning_.store(!settings_.frame.enabled || settings_.frame.autoStart);
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
        // 数据目的：仅关联生效数据源的 sink 生效（帧数据源才有原始帧可转发）
        for (const auto& k : settings_.frame.sinks) {
            if (k.sourceName != settings_.frame.sourceName) continue;
            SinkLink sk;
            sk.cfg = k;
            sinks_.push_back(std::move(sk));
        }
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
        // 手动停止（或 autoStart 关的初始态）：保持断开，标签报未启动，等待重新开启
        if (!sourceRunning_.load()) {
            if (ds->isConnected()) {
                ds->disconnect();
                connected_ = false;
            }
            for (auto& sk : sinks_) { // 数据目的同步断开（重启后随帧自动重连）
                if (!sk.up) continue;
                sk.up = false;
                if (sk.cfg.serial) sk.serial->close();
                else if (sk.cfg.udp) sk.udp->stop();
                else sk.tcp->disconnect();
            }
            failAll("数据源未启动");
            for (int slept = 0; slept < 500 && !stopFlag_; slept += 20)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
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

        // 1.5) 帧数据源：转发原始帧给数据目的，再喂 UI 报文监视
        if (frameSource_) {
            std::deque<FrameDataSource::FrameLogEntry> frames;
            frameSource_->drainFrameLog(frames);
            if (!frames.empty()) {
                forwardFrames(frames);
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
    sinks_.clear();
}

// 建立 sink 链路（按传输建对应连接；UDP 客户端 connect、服务端 bind 后向最近对端发）
void PollWorker::connectSink(SinkLink& sk, std::string& err) {
    const FrameSinkSettings& k = sk.cfg;
    if (k.serial) {
        if (!sk.serial) sk.serial = std::make_unique<packet::SerialLink>();
        sk.up = sk.serial->open(k.serialPort, k.baud, k.dataBits, k.parity, k.stopBits, err);
    } else if (k.udp) {
        if (!sk.udp) sk.udp = std::make_unique<packet::UdpLink>();
        sk.up = k.udpClient ? sk.udp->startClient(k.host, k.remotePort, err)
                            : sk.udp->start(k.localPort, err);
    } else {
        if (!sk.tcp) sk.tcp = std::make_unique<packet::TcpLink>();
        sk.up = k.tcpClient ? sk.tcp->connect(k.host, k.remotePort, err)
                            : sk.tcp->listen(k.localPort, err);
    }
}

// 把数据源收到的原始帧原样转发到各 sink（尽力而为：断线丢帧、2s 退避重连）
void PollWorker::forwardFrames(const std::deque<FrameDataSource::FrameLogEntry>& frames) {
    if (sinks_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    for (auto& sk : sinks_) {
        if (!sk.up && now < sk.nextTry) continue;
        std::string err;
        if (!sk.up) {
            connectSink(sk, err);
            if (!sk.up) {
                sk.nextTry = now + std::chrono::seconds(2);
                continue; // 本批帧丢弃（转发不缓存）
            }
        }
        bool ok = true;
        for (const auto& f : frames) {
            if (sk.cfg.serial)
                ok = sk.serial->send(f.data, err) && ok;
            else if (sk.cfg.udp)
                ok = sk.udp->send(f.data, err) && ok;
            else
                ok = sk.tcp->send(f.data, err) && ok;
            if (!ok) break;
        }
        if (!ok) { // 断线：关链路，等下批帧再重连
            sk.up = false;
            sk.nextTry = now + std::chrono::seconds(2);
            if (sk.cfg.serial) sk.serial->close();
            else if (sk.cfg.udp) sk.udp->stop();
            else sk.tcp->disconnect();
        }
    }
}

} // namespace softg::viewer
