#include "base/data/frame/FrameDataSource.h"

#include "base/packet/HexUtil.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace pv {

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
// Bool→bool、String/Enum/Hex→std::string（枚举名/原数值文本/十六进制文本），不做换算
bool fieldValue(const TagField& f, const uint8_t* p, int avail, TagValue& out) {
    int bytes = packet::fieldTypeBytes(f.type);
    if (f.type == packet::FieldType::String || f.type == packet::FieldType::Enum ||
        f.type == packet::FieldType::Hex)
        bytes = f.bytes; // 长度/宽度可配
    if (bytes <= 0) return false;
    if (f.offset < 0 || (int64_t)f.offset + bytes > (int64_t)avail) return false;
    p += f.offset;
    switch (f.type) {
    case packet::FieldType::Bool:
        out = (p[0] != 0);
        return true;
    case packet::FieldType::Hex: { // 十六进制文本（大写、空格分隔，与报文监视同格式）
        static const char kHex[] = "0123456789ABCDEF";
        std::string s;
        s.reserve((size_t)bytes * 3 - 1);
        for (int i = 0; i < bytes; ++i) {
            if (i) s += ' ';
            s += kHex[p[i] >> 4];
            s += kHex[p[i] & 0xF];
        }
        out = std::move(s);
        return true;
    }
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
    // 解析值缓存按 address 去重分组（同地址多字段共享一组槽位，顺序写入后到者覆盖）
    groupOf_.resize(cfg_.fields.size());
    for (size_t fi = 0; fi < cfg_.fields.size(); ++fi) {
        int addr = cfg_.fields[fi].address;
        int g = -1;
        for (size_t k = 0; k < groupAddr_.size(); ++k)
            if (groupAddr_[k] == addr) g = (int)k;
        if (g < 0) {
            groupAddr_.push_back(addr);
            g = (int)groupAddr_.size() - 1;
        }
        groupOf_[fi] = g;
    }
    latestByGroup_.resize(groupAddr_.size());
    groupSet_.assign(groupAddr_.size(), 0);
}

FrameDataSource::~FrameDataSource() { disconnect(); }

bool FrameDataSource::connect(std::string& err) {
    disconnect();
    // 协议组多帧头：拆帧器按各帧头匹配；否则单配置（逐流拆帧器同配置）
    framings_ = cfg_.framings;
    if (framings_.empty()) framings_.push_back(cfg_.framing);
    if (cfg_.framings.size() > 1) {
        splitter_.setConfigs(cfg_.framings);
        flows_.setConfigs(cfg_.framings);
    } else {
        splitter_.setConfig(cfg_.framing);
        flows_.setConfig(cfg_.framing);
    }
    // 分组计数向量按拆帧配置数就位（重连不重置：与 matchedFrames_ 累计口径一致）
    if (matchedByIdx_.size() != framings_.size())
        matchedByIdx_.assign(framings_.size(), 0);
    if (cfg_.serial) // 串口：打开 COM 口（字节流，与 TCP 共用拆帧）
        return serial_.open(cfg_.serialPort, cfg_.baud, cfg_.dataBits, cfg_.parity,
                            cfg_.stopBits, err);
    if (cfg_.listen) { // 监听：三元组 dip/dport/协议 过滤，绑定端口即 dport
        if (cfg_.listenPcap) // 镜像抓包：BPF 过滤任一方向命中（未装 Npcap 时 err 带安装提示）
            return pcap_.start(cfg_.listenNic,
                               packet::buildListenBpf(cfg_.listenIp, cfg_.listenPort,
                                                      cfg_.listenTcp),
                               err);
        if (cfg_.listenTcp)
            return tcp_.listen(cfg_.listenPort, err, cfg_.listenIp);
        // UDP 反向命中 = 源==(dip,dport)；dip="*"（通配）不限源端口（正向全收）
        int matchPort = cfg_.listenIp == "*" ? 0 : cfg_.listenPort;
        return udp_.start(cfg_.listenPort, err, cfg_.listenIp, matchPort);
    }
    if (cfg_.udp) {
        if (cfg_.udpMulticast) // 组播：绑定组端口并加入组（host=组地址）
            return udp_.startMulticast(cfg_.host, cfg_.localPort, err);
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
    serial_.close();
    pcap_.stop();
    flows_.reset();
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(groupSet_.begin(), groupSet_.end(), 0); // 解析值随连接失效
}

bool FrameDataSource::isConnected() const {
    if (cfg_.serial) return serial_.isOpen();
    if (cfg_.listen) {
        if (cfg_.listenPcap) return pcap_.isRunning();
        return cfg_.listenTcp ? tcp_.isConnected() : udp_.isRunning();
    }
    return cfg_.udp ? udp_.isRunning() : tcp_.isConnected();
}

void FrameDataSource::pumpFrames() {
    std::vector<std::vector<uint8_t>> frames;

    // 监听与 UDP 是并列来源：监听协议=UDP 时同样走 UDP 收包。
    // 只看 cfg_.udp 会漏收监听帧（transport=监听 时 cfg_.udp 为 false）
    bool useUdp = cfg_.listen ? !cfg_.listenTcp : cfg_.udp;
    if (cfg_.listen && cfg_.listenPcap) {
        // 镜像抓包：UDP 数据报天然成帧；TCP 逐流拆帧（SPAN 双向流不互相掺杂）
        std::deque<packet::CapPacket> in;
        pcap_.drain(in);
        while (!in.empty()) {
            flows_.feed(in.front(), frames);
            in.pop_front();
        }
    } else if (useUdp) {
        std::deque<packet::UdpPacket> in;
        udp_.drain(in);
        while (!in.empty()) {
            frames.push_back(std::move(in.front().data));
            in.pop_front();
        }
    } else if (cfg_.serial) { // 串口：字节流，与 TCP 同路拆帧
        std::deque<packet::TcpChunk> in;
        serial_.drain(in);
        while (!in.empty()) {
            splitter_.feed(in.front().data.data(), in.front().data.size(), frames);
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

    // 时间戳按批共用：显示为 HH:MM:SS 秒级，逐帧调 localtime_s 是热路径无谓开销
    std::string ts = nowTimeText();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& frame : frames) {
        // 结构解析：TLV 模式按 T 匹配字段、偏移相对负载；帧头+Length 相对整帧。
        // 多帧头（协议组）时逐配置试解，命中者决定哪些协议的字段参与——
        // AA55 帧只驱动 AA55 协议的字段，AA56 帧只驱动 AA56 协议的字段
        int64_t tagId = -1;
        const uint8_t* payload = nullptr;
        int payloadLen = 0;
        int matchedIdx = 0;
        bool structured = false;
        for (size_t k = 0; k < framings_.size(); ++k) {
            if (packet::decodeFrameOnce(framings_[k], frame, tagId, payload, payloadLen)) {
                structured = true;
                matchedIdx = (int)k;
                break;
            }
        }
        bool matched = false; // 符合协议：成帧且至少一个字段可解出
        if (structured) {
            for (size_t fi = 0; fi < cfg_.fields.size(); ++fi) {
                const TagField& f = cfg_.fields[fi];
                if (cfg_.framing.mode == packet::FrameMode::Tlv && f.tagId != tagId)
                    continue; // TLV：字段按槽位标识匹配帧
                if (cfg_.framings.size() > 1 && f.framingIndex != matchedIdx)
                    continue; // 多帧头：字段只由其归属协议的帧驱动
                TagValue val;
                if (fieldValue(f, payload, payloadLen, val)) {
                    int g = groupOf_[fi];
                    latestByGroup_[g] = std::move(val); // 同槽位多字段：后到者覆盖
                    groupSet_[g] = 1;
                    matched = true;
                }
            }
        }
        lastFrameTime_ = ts;
        if (matched) {
            ++matchedFrames_;
            if (matchedIdx < (int)matchedByIdx_.size())
                ++matchedByIdx_[matchedIdx]; // 各帧头归属分别计数（协议卡显示自己的）
        }
        // 转发队列持有原帧缓冲（move 零拷贝）；监视日志留独立副本、满 200 裁最旧。
        // 两条队列解耦：高帧率下日志裁剪不得波及数据目的转发
        FrameLogEntry e;
        e.timeText = ts;
        e.data = frame;
        frameLog_.push_back(std::move(e));
        while (frameLog_.size() > 200)
            frameLog_.pop_front();
        forward_.push_back(std::move(frame));
        while (forward_.size() > 100000)
            forward_.pop_front(); // 防御性上限（worker 停摆时防内存无限增长）
    }
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

        // address → 地址组（字段数有限，线性扫描即可；未收到数据 = Bad）
        int g = -1;
        for (size_t k = 0; k < groupAddr_.size(); ++k)
            if (groupAddr_[k] == t->address) g = (int)k;
        if (g < 0 || !groupSet_[g]) {
            r.ok = false;
            r.quality = TagQuality::Bad;
            r.error = "等待报文数据";
            continue;
        }
        const TagValue& raw = latestByGroup_[g];
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

void FrameDataSource::drainForwardFrames(std::deque<std::vector<uint8_t>>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!forward_.empty()) {
        out.push_back(std::move(forward_.front()));
        forward_.pop_front();
    }
}

std::string FrameDataSource::lastFrameTimeText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrameTime_;
}

uint64_t FrameDataSource::matchedFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return matchedFrames_;
}

std::map<int, uint64_t> FrameDataSource::matchedFrameCountByIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<int, uint64_t> out;
    for (size_t k = 0; k < matchedByIdx_.size(); ++k)
        out[(int)k] = matchedByIdx_[k];
    return out;
}

} // namespace pv
