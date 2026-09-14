// 性能测试：协议解析管线吞吐 + 数据源→数据目的 UDP 回环转发吞吐。
// 仅 Release 构建运行（NDEBUG）：Debug 无优化数值无参考意义，按惯例 [skip]。
// 输出参考值并以保守下限断言：下限取本机 Release 测量值的约 1/7（慢速 CI 也可过，
// 数量级退化——如意外引入逐帧拷贝/频繁分配——才判失败）。
// 回环端口绑定失败按惯例 [skip]（其余功能测试不受影响）。
#include "PvTest.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <thread>

#include "base/data/frame/FrameDataSource.h"
#include "base/data/frame/TestFrameGen.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/FrameCodec.h"
#include "base/packet/NpcapApi.h"
#include "base/packet/PacketSpec.h"
#include "base/packet/UdpLink.h"

using namespace pv;

namespace {

double secondsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// 批间短自旋间隙：让收包线程跟上（sleep 受 Windows 15.6ms 定时器分辨率限制）
void spinGapUs(int us) {
    auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) std::this_thread::yield();
}

// 泵站上行帧样板：帧头+Length（AA 55 | len(2B,BE,负载长) | 负载 32B，整帧 36B）。
// 10 个字段覆盖全部类型路径：整数/浮点/scale 换算/布尔/枚举/字符串
void makeSampleSpec(packet::FramingConfig& fr, std::vector<TagField>& fields) {
    fr.mode = packet::FrameMode::HeaderLength;
    fr.header = {0xAA, 0x55};
    fr.lenOffset = 2;
    fr.lenBytesHeader = 2;
    fr.bigEndianHeader = true;
    fr.lenIncludesAll = false; // length 只计负载

    auto add = [&](const char* name, int offset, const char* type, int bytes,
                   double scale = 1.0,
                   std::vector<std::pair<int64_t, std::string>> enums = {}) {
        TagField f;
        f.name = name;
        f.offset = offset;
        packet::fieldTypeFromString(type, f.type);
        f.bytes = bytes;
        f.scale = scale;
        f.enums = std::move(enums);
        f.address = (int)fields.size(); // 直接构造 settings 需显式槽位
        fields.push_back(f);
    };
    add("温度", 0, "u16", 2, 0.1);
    add("湿度", 2, "u16", 2, 0.1);
    add("压力", 4, "f32", 4);
    add("流量", 8, "f32", 4);
    add("计数", 12, "u32", 4);
    add("阀位", 16, "u16", 2);
    add("运行", 18, "bool", 1);
    add("模式", 19, "enum", 1, 1.0, {{0, "手动"}, {1, "自动"}, {2, "远程"}});
    add("名称", 20, "string", 8);
    add("电流", 28, "f32", 4);
}

} // namespace

TEST_CASE("性能：协议解析管线（拆帧+结构解+字段解析）") {
#ifndef NDEBUG
    std::printf("    [skip] 性能测试仅在 Release 构建运行（Debug 未优化，数值无参考意义）\n");
    return;
#endif
    packet::FramingConfig fr;
    std::vector<TagField> fields;
    makeSampleSpec(fr, fields);

    const int N = 20000;
    auto frames = generateTestFrames(fr, fields, N, 7);
    REQUIRE(frames.size() == (size_t)N);

    // 拼成字节流（模拟 TCP/串口串流），按 1448B 分段喂拆帧器（模拟 MSS 分段）
    size_t streamLen = 0;
    for (const auto& f : frames) streamLen += f.size();
    std::vector<uint8_t> stream;
    stream.reserve(streamLen);
    for (const auto& f : frames) stream.insert(stream.end(), f.begin(), f.end());

    // 展示字段（报文监视解析路径）：parsePacket 收整帧，偏移 = 负载偏移 + 帧头/长度 4B
    std::vector<packet::PacketField> disp;
    for (const auto& f : fields) {
        packet::PacketField d;
        d.name = f.name;
        d.offset = f.offset + 4;
        d.length = f.bytes;
        d.type = f.type;
        d.bigEndian = f.bigEndian;
        d.scale = f.scale;
        d.enums = f.enums;
        disp.push_back(d);
    }

    packet::FrameSplitter sp(fr);
    std::vector<std::vector<uint8_t>> out;
    out.reserve(N);
    size_t decoded = 0, parsedOk = 0;
    const size_t kChunk = 1448;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t off = 0; off < stream.size(); off += kChunk) {
        size_t n = std::min(kChunk, stream.size() - off);
        sp.feed(stream.data() + off, n, out);
    }
    for (const auto& f : out) {
        int64_t tagId = -1;
        const uint8_t* payload = nullptr;
        int payloadLen = 0;
        if (!packet::decodeFrameOnce(fr, f, tagId, payload, payloadLen)) continue;
        ++decoded;
        for (const auto& r : packet::parsePacket(disp, f))
            if (r.ok) ++parsedOk;
    }
    double sec = secondsSince(t0);

    REQUIRE(out.size() == (size_t)N); // 拆帧零丢零多
    CHECK(decoded == (size_t)N);      // 结构解全部通过
    CHECK(parsedOk == (size_t)N * disp.size()); // 每字段都解出（无越界）

    double fps = N / sec;
    double mbps = (double)streamLen / sec / 1024.0 / 1024.0;
    std::printf("    [perf] 协议解析 %.0f 帧/秒（%.1f MB/s；%dB/帧 × %d 帧，耗时 %.0f ms）\n",
                fps, mbps, (int)frames[0].size(), N, sec * 1000.0);
    // 本机 x64-release 参考约 24 万帧/秒（8.5 MB/s）；下限取约 1/8，防数量级退化
    CHECK(fps >= 30000.0);
}

TEST_CASE("性能：数据源→数据目的 UDP 回环转发") {
#ifndef NDEBUG
    std::printf("    [skip] 性能测试仅在 Release 构建运行（Debug 未优化，数值无参考意义）\n");
    return;
#endif
    packet::FramingConfig fr;
    std::vector<TagField> fields;
    makeSampleSpec(fr, fields);

    const int N = 20000;
    // 分批发送+转发：小批量突发 + 批间短间隙，避免溢出内核收包缓冲造成 UDP 丢包
    //（丢包属传输而非转发语义，性能测试须零丢失对账；转发队列已与 200 条上限的
    // 监视日志解耦。UdpLink 已加大 SO_RCVBUF，批 200 + 间隙 100µs 实测零丢失）
    const int batch = 200;
    auto frames = generateTestFrames(fr, fields, N, 11);
    REQUIRE(frames.size() == (size_t)N);

    // 数据源：UDP 服务端绑定 59360（帧格式同上，UDP 数据报天然成帧）
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = true;
    cfg.localPort = 59360;
    cfg.framing = fr;
    cfg.fields = fields;
    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59360 绑定失败: %s\n", err.c_str());
        return;
    }

    // 平台侧接收端 + 数据目的（UDP 客户端 connect 59361，转发方向明确）
    packet::UdpLink receiver;
    if (!receiver.start(59361, err)) {
        std::printf("    [skip] 端口 59361 绑定失败: %s\n", err.c_str());
        src.disconnect();
        return;
    }
    packet::UdpLink sink;
    REQUIRE(sink.startClient("127.0.0.1", 59361, err));

    // 模拟设备：向数据源端口发帧
    packet::UdpLink sender;
    REQUIRE(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59360);

    // 读标签集（槽位 = 字段序号；字符串/枚举 → String，布尔 → Bool，数值 → Float32）
    std::vector<Tag> tags;
    for (const auto& f : fields) {
        Tag t;
        t.name = f.name;
        t.address = f.address;
        if (f.type == packet::FieldType::Bool) t.type = TagDataType::Bool;
        else if (f.type == packet::FieldType::String ||
                 f.type == packet::FieldType::Enum)
            t.type = TagDataType::String;
        else
            t.type = TagDataType::Float32; // 换算在字段侧，标签不二次缩放
        tags.push_back(t);
    }
    std::vector<const Tag*> tagPtrs;
    for (auto& t : tags) tagPtrs.push_back(&t);

    uint64_t received = 0, rxBytes = 0;
    bool firstByteIdentical = false;
    std::deque<packet::UdpPacket> rx;
    auto drainRx = [&]() {
        receiver.drain(rx);
        for (auto& p : rx) {
            if (received == 0) firstByteIdentical = (p.data == frames[0]); // 原样转发（回环保序）
            ++received;
            rxBytes += p.data.size();
        }
        rx.clear();
    };

    auto t0 = std::chrono::steady_clock::now();
    size_t sent = 0;
    for (int b = 0; b < N / batch; ++b) {
        for (int i = 0; i < batch; ++i, ++sent)
            REQUIRE(sender.send(frames[sent], err));
        // 批间短暂间隙让收包线程跟上内核队列（原因见 spinGapUs 注释）
        spinGapUs(100);
        // PollWorker 同款路径：readTags 驱动收包+解析，转发队列经数据目的原样转发
        //（转发已与 200 条上限的监视日志解耦，走专用 drainForwardFrames）
        (void)src.readTags(tagPtrs);
        std::deque<std::vector<uint8_t>> toForward;
        src.drainForwardFrames(toForward);
        for (const auto& f : toForward) CHECK(sink.send(f, err));
        drainRx();
    }
    // 收尾：接收端收包线程异步，轮询等齐（上限 2s；超时即丢包，对账 CHECK 会报）
    for (int i = 0; i < 200 && received < (uint64_t)N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        drainRx();
    }
    double sec = secondsSince(t0);

    // 全链路对账：平台侧帧数/字节数与发送一致、内容原样、解析计数齐全
    std::printf("    [perf] 对账: 发送 %d，平台收到 %llu，数据源解析命中 %llu\n",
                N, (unsigned long long)received,
                (unsigned long long)src.matchedFrameCount());
    CHECK(received == (uint64_t)N);
    CHECK(rxBytes == (uint64_t)N * frames[0].size());
    CHECK(firstByteIdentical);
    CHECK(src.matchedFrameCount() == (uint64_t)N);

    auto results = src.readTags(tagPtrs); // 顺带验证标签值可读
    for (const auto& r : results) CHECK(r.ok);

    double fps = N / sec;
    double mbps = (double)rxBytes / sec / 1024.0 / 1024.0;
    std::printf("    [perf] 数据源→数据目的转发 %.0f 帧/秒（%.1f MB/s；%dB/帧 × %d 帧，耗时 %.0f ms）\n",
                fps, mbps, (int)frames[0].size(), N, sec * 1000.0);
    // 本机 x64-release（虚拟机）参考约 4 万帧/秒，后台负载波动时可到 2.7 万仍零丢失；
    // 剩余瓶颈为每帧两跳 ~10µs 的回环 syscall（UDP 数据报无法合并发送）。
    // 下限取约 1/4，防数量级退化
    CHECK(fps >= 10000.0);

    sender.stop();
    sink.stop();
    receiver.stop();
    src.disconnect();
}

TEST_CASE("性能：回环发包对比 sendto vs Npcap pcap_sendpacket") {
#ifndef NDEBUG
    std::printf("    [skip] 性能测试仅在 Release 构建运行（Debug 未优化，数值无参考意义）\n");
    return;
#endif
    // 门控：Npcap 可用且能找到回环设备（\Device\NPF_Loopback）
    std::string err;
    const packet::npcap::Api* api = packet::npcap::instance(err);
    if (!api) {
        std::printf("    [skip] %s\n", err.c_str());
        return;
    }
    std::string loopName;
    {
        packet::npcap::PcapIf* devs = nullptr;
        char ebuf[256] = {};
        if (api->findalldevs(&devs, ebuf) != 0) {
            std::printf("    [skip] 枚举网卡失败: %s\n", ebuf);
            return;
        }
        for (auto* it = devs; it; it = it->next) {
            std::string n = it->name ? it->name : "";
            if (n.find("Loopback") != std::string::npos) {
                loopName = n;
                break;
            }
        }
        api->freealldevs(devs);
    }
    if (loopName.empty()) {
        std::printf("    [skip] 未找到 Npcap 回环设备（安装 Npcap 时需勾选回环支持）\n");
        return;
    }
    char ebuf[256] = {};
    auto* pc = api->open_live(loopName.c_str(), 65535, 0, 100, ebuf);
    if (!pc) {
        std::printf("    [skip] 回环设备打开失败: %s\n", ebuf);
        return;
    }
    const int dl = api->datalink(pc);
    if (dl != 0 && dl != 12) { // 0=DLT_NULL（4B 族头）/ 12=DLT_RAW（无链路头）
        std::printf("    [skip] 回环链路层类型 %d 暂不支持\n", dl);
        api->close(pc);
        return;
    }

    const int kPort = 59363, kSport = 59364;
    const int N = 20000, batch = 100;
    // 36B 样板负载（与转发测试同规格的帧头+Length 帧）
    const uint8_t payload[36] = {0xAA, 0x55, 0x00, 0x20};
    std::vector<uint8_t> dat(payload, payload + sizeof(payload));

    // Npcap 注入模板：链路头 + IPv4(20B, 校验和) + UDP(8B, 校验和=0 合法) + 负载
    std::vector<uint8_t> pkt;
    if (dl == 0) { // DLT_NULL：4 字节主机字节序 AF_INET
        const uint8_t fam[] = {2, 0, 0, 0};
        pkt.insert(pkt.end(), fam, fam + 4);
    }
    const size_t ipOff = pkt.size();
    const uint16_t totalLen = 20 + 8 + (uint16_t)sizeof(payload);
    const uint8_t ipHead[20] = {
        0x45, 0x00, (uint8_t)(totalLen >> 8), (uint8_t)totalLen,
        0x00, 0x00, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00,
        127, 0, 0, 1, 127, 0, 0, 1};
    pkt.insert(pkt.end(), ipHead, ipHead + 20);
    const uint16_t udpLen = 8 + (uint16_t)sizeof(payload);
    const uint8_t udpHead[8] = {
        (uint8_t)(kSport >> 8), (uint8_t)kSport, (uint8_t)(kPort >> 8), (uint8_t)kPort,
        (uint8_t)(udpLen >> 8), (uint8_t)udpLen, 0x00, 0x00};
    pkt.insert(pkt.end(), udpHead, udpHead + 8);
    pkt.insert(pkt.end(), payload, payload + sizeof(payload));
    { // IPv4 头校验和（回环路径通常不校验，构全以防万一）
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2)
            sum += (pkt[ipOff + i] << 8) | pkt[ipOff + i + 1];
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        uint16_t cks = (uint16_t)(~sum);
        pkt[ipOff + 10] = (uint8_t)(cks >> 8);
        pkt[ipOff + 11] = (uint8_t)cks;
    }

    packet::UdpLink receiver;
    if (!receiver.start(kPort, err)) {
        std::printf("    [skip] 端口 %d 绑定失败: %s\n", kPort, err.c_str());
        api->close(pc);
        return;
    }
    std::deque<packet::UdpPacket> rx;
    auto drainCount = [&](uint64_t& n) {
        receiver.drain(rx);
        n += rx.size();
        rx.clear();
    };
    auto waitQuiesce = [&]() { // 等收包线程清空（换路前不串数）
        for (int i = 0; i < 30; ++i) {
            receiver.drain(rx);
            bool empty = rx.empty();
            rx.clear();
            if (empty) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    // 路径 A：Winsock sendto（现行转发路径同款）
    uint64_t gotA = 0;
    double secA = 0;
    {
        packet::UdpLink snd;
        REQUIRE(snd.start(0, err));
        snd.setRemote("127.0.0.1", kPort);
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < N / batch; ++b) {
            for (int i = 0; i < batch; ++i) CHECK(snd.send(dat, err));
            spinGapUs(300);
            drainCount(gotA);
        }
        secA = secondsSince(t0);
        for (int i = 0; i < 200 && gotA < (uint64_t)N; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            drainCount(gotA);
        }
        snd.stop();
    }
    waitQuiesce();

    // 路径 B：Npcap pcap_sendpacket 驱动级注入（手工 IP/UDP 头）
    uint64_t gotB = 0;
    double secB = 0;
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < N / batch; ++b) {
            for (int i = 0; i < batch; ++i)
                CHECK(api->sendpacket(pc, pkt.data(), (int)pkt.size()) == 0);
            spinGapUs(300);
            drainCount(gotB);
        }
        secB = secondsSince(t0);
        for (int i = 0; i < 200 && gotB < (uint64_t)N; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            drainCount(gotB);
        }
    }

    CHECK(gotA == (uint64_t)N); // 基线路径必须零丢失（否则测量无效）
    std::printf("    [perf] sendto        %.0f 帧/秒（送达 %llu/%d，%.0f ms）\n",
                N / secA, (unsigned long long)gotA, N, secA * 1000.0);
    std::printf("    [perf] pcap_sendpkt  %.0f 帧/秒（送达 %llu/%d，%.0f ms）\n",
                N / secB, (unsigned long long)gotB, N, secB * 1000.0);
    // Npcap 路径不设硬阈值：能否送达协议栈取决于驱动/系统配置，本用例做对比观测

    api->close(pc);
    receiver.stop();
}
