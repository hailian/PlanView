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

// 按字段类型解出 TagValue；越界/不支持返回 false
// 整数→int64_t、浮点→double（数值字段乘 scale 换算，scale=1 保持整数型）；
// Bool→bool、String/Enum→std::string（枚举名或原数值文本），这两类不做换算
bool fieldValue(const TagField& f, const uint8_t* p, int avail, TagValue& out) {
    int bytes = packet::fieldTypeBytes(f.type);
    if (f.type == packet::FieldType::String || f.type == packet::FieldType::Enum)
        bytes = f.bytes; // 长度/宽度可配
    if (bytes <= 0) return false;
    if (f.offset < 0 || (int64_t)f.offset + bytes > (int64_t)avail) return false;
    p += f.offset;
    switch (f.type) {
    case packet::FieldType::Bool:
        out = (p[0] != 0);
        return true;
    case packet::FieldType::String: {
        int n = bytes; // 去尾部 0x00/0xFF 填充，保留原始字节（UTF-8 友好）
        while (n > 0 && (p[n - 1] == 0x00 || p[n - 1] == 0xFF)) --n;
        out = std::string((const char*)p, (size_t)n);
        return true;
    }
    case packet::FieldType::Enum: {
        uint64_t u = readUint(p, bytes, f.bigEndian);
        if (const std::string* nm = packet::findEnumName(f.enums, (int64_t)u)) {
            out = *nm;
        } else {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)u);
            out = std::string(buf);
        }
        return true;
    }
    case packet::FieldType::F32: {
        uint32_t u = (uint32_t)readUint(p, 4, f.bigEndian);
        float v;
        std::memcpy(&v, &u, 4);
        out = (double)v * f.scale;
        return true;
    }
    case packet::FieldType::F64: {
        uint64_t u = readUint(p, 8, f.bigEndian);
        double v;
        std::memcpy(&v, &u, 8);
        out = v * f.scale;
        return true;
    }
    default: { // 整数：有符号补码扩展，统一升为 int64_t；带 scale 时乘出工程值（可能为小数）
        uint64_t u = readUint(p, bytes, f.bigEndian);
        int bits = bytes * 8;
        if ((f.type == packet::FieldType::I8 || f.type == packet::FieldType::I16 ||
             f.type == packet::FieldType::I32) &&
            bytes < 8 && (u & (1ULL << (bits - 1))))
            u |= ~0ULL << bits;
        if (f.scale != 1.0)
            out = (double)u * f.scale;
        else
            out = (int64_t)u;
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
    if (cfg_.udp) {
        if (cfg_.udpClient)
            return udp_.startClient(cfg_.host, cfg_.remotePort, err);
        return udp_.start(cfg_.localPort, err);
    }

    // TCP：客户端连接远端 / 服务端监听本地（失败交由 PollWorker 退避重试）
    if (cfg_.tcpClient)
        return tcp_.connect(cfg_.host, cfg_.remotePort, err);
    return tcp_.listen(cfg_.localPort, err);
}

void FrameDataSource::disconnect() {
    udp_.stop();
    tcp_.disconnect();
    std::lock_guard<std::mutex> lock(mutex_);
    latestValue_.clear();
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
                TagValue val;
                if (fieldValue(f, payload, payloadLen, val))
                    latestValue_[f.address] = std::move(val); // 同槽位多字段：后到者覆盖
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

        auto it = latestValue_.find(t->address);
        if (it == latestValue_.end()) {
            r.ok = false;
            r.quality = TagQuality::Bad;
            r.error = "等待报文数据";
            continue;
        }
        const TagValue& raw = it->second;
        if (auto* s = std::get_if<std::string>(&raw)) {
            r.value = *s; // 字符串/枚举名：直通
        } else if (auto* b = std::get_if<bool>(&raw)) {
            r.value = *b;
        } else {
            double rawNum = 0;
            if (auto* i = std::get_if<int64_t>(&raw)) rawNum = (double)*i;
            else if (auto* d = std::get_if<double>(&raw)) rawNum = *d;
            double eng = t->toEngineering(rawNum); // 数值走工程换算
            if (t->type == TagDataType::Bool) {
                r.value = eng != 0;
            } else {
                r.value = (eng == std::floor(eng) && std::abs(eng) < 9.0e15)
                              ? TagValue((int64_t)eng)
                              : TagValue(eng);
            }
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
