#include "base/data/frame/FrameDataSource.h"

#include "base/packet/HexUtil.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace softg {

namespace {

std::string nowTimeText() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

uint64_t readUint(const uint8_t* p, int n, bool bigEndian) {
    uint64_t v = 0;
    if (bigEndian)
        for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    else
        for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// 按字段类型解出原始数值；hex/ascii 非数值返回 false
bool fieldRawDouble(const TagField& f, const uint8_t* p, int avail, double& out) {
    int bytes = packet::fieldTypeBytes(f.type);
    if (bytes <= 0) return false;               // hex/ascii
    if (f.offset < 0 || (int64_t)f.offset + bytes > (int64_t)avail) return false;
    p += f.offset;
    switch (f.type) {
    case packet::FieldType::F32: {
        uint32_t u = (uint32_t)readUint(p, 4, f.bigEndian);
        float v;
        std::memcpy(&v, &u, 4);
        out = v;
        return true;
    }
    case packet::FieldType::F64: {
        uint64_t u = readUint(p, 8, f.bigEndian);
        double v;
        std::memcpy(&v, &u, 8);
        out = v;
        return true;
    }
    default: {
        uint64_t u = readUint(p, bytes, f.bigEndian);
        int bits = bytes * 8;
        if (f.type == packet::FieldType::I8 || f.type == packet::FieldType::I16 ||
            f.type == packet::FieldType::I32) {
            if (bytes < 8 && (u & (1ULL << (bits - 1)))) u |= ~0ULL << bits;
            out = (double)(int64_t)u;
        } else {
            out = (double)u;
        }
        return true;
    }
    }
}

} // namespace

FrameDataSource::FrameDataSource(FrameSourceSettings settings) : cfg_(std::move(settings)) {
    // 固定长度类型的字节数以类型为准
    for (auto& f : cfg_.fields)
        if (int n = packet::fieldTypeBytes(f.type)) f.bytes = n;
}

FrameDataSource::~FrameDataSource() { disconnect(); }

bool FrameDataSource::connect(std::string& err) {
    disconnect();
    splitter_.setConfig(cfg_.framing);
    if (cfg_.udp)
        return udp_.start(cfg_.localPort, err);

    if (!tcp_.connect(cfg_.host, cfg_.remotePort, err)) {
        // TCP 连接失败交由 PollWorker 退避重试；构造状态保持干净
        return false;
    }
    return true;
}

void FrameDataSource::disconnect() {
    udp_.stop();
    tcp_.disconnect();
    std::lock_guard<std::mutex> lock(mutex_);
    latestRaw_.clear();
}

bool FrameDataSource::isConnected() const {
    return cfg_.udp ? udp_.isRunning() : tcp_.isConnected();
}

void FrameDataSource::pumpFrames() {
    std::vector<std::vector<uint8_t>> frames;

    if (cfg_.udp) {
        std::deque<packet::UdpPacket> in;
        udp_.drain(in);
        while (!in.empty()) {
            frames.push_back(std::move(in.front().data));
            in.pop_front();
        }
    } else {
        std::deque<packet::TcpChunk> in;
        tcp_.drain(in);
        while (!in.empty()) {
            splitter_.feed(in.front().data.data(), in.front().data.size(), frames);
            in.pop_front();
        }
    }

    if (frames.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& frame : frames) {
        // 结构解析：TLV 模式按 T 匹配字段、偏移相对负载；帧头+Length 相对整帧
        int64_t tagId = -1;
        const uint8_t* payload = nullptr;
        int payloadLen = 0;
        bool structured = packet::decodeFrameOnce(cfg_.framing, frame, tagId, payload, payloadLen);
        if (structured) {
            for (const auto& f : cfg_.fields) {
                if (cfg_.framing.mode == packet::FrameMode::Tlv && f.tagId != tagId)
                    continue; // TLV：字段按槽位标识匹配帧
                double raw = 0;
                if (fieldRawDouble(f, payload, payloadLen, raw))
                    latestRaw_[f.address] = raw; // 同槽位多字段：后到者覆盖
            }
        }
        FrameLogEntry e;
        e.timeText = nowTimeText();
        e.data = std::move(frame);
        frameLog_.push_back(std::move(e));
    }
    while (frameLog_.size() > 200)
        frameLog_.pop_front();
}

std::vector<TagReadResult> FrameDataSource::readTags(const std::vector<const Tag*>& tags) {
    pumpFrames();

    std::vector<TagReadResult> results(tags.size());
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < tags.size(); ++i) {
        const Tag* t = tags[i];
        if (!t) continue;
        TagReadResult& r = results[i];
        r.tag = t->name;

        auto it = latestRaw_.find(t->address);
        if (it == latestRaw_.end()) {
            r.ok = false;
            r.quality = TagQuality::Bad;
            r.error = "等待报文数据";
            continue;
        }
        double eng = t->toEngineering(it->second);
        if (t->type == TagDataType::Bool) {
            r.value = eng != 0;
        } else {
            r.value = (eng == std::floor(eng) && std::abs(eng) < 9.0e15)
                          ? TagValue((int64_t)eng)
                          : TagValue(eng);
        }
        r.ok = true;
        r.quality = TagQuality::Good;
    }
    return results;
}

bool FrameDataSource::writeTag(const Tag&, TagValue, std::string& err) {
    err = "帧数据源暂不支持写回";
    return false;
}

void FrameDataSource::drainFrameLog(std::deque<FrameLogEntry>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!frameLog_.empty()) {
        out.push_back(std::move(frameLog_.front()));
        frameLog_.pop_front();
    }
}

} // namespace softg
