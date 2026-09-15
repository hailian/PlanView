// PollWorker 单测：worker 循环语义（收包泵 / 数据目的转发 / 标签刷新）。
// 回归场景：UDP 服务端数据源（autoStart）+ TCP 客户端数据目的、工程无任何标签——
// 此前 viewer 在无标签时不启动 worker、且 worker 循环零标签不泵收包，
// 表现为「收不到数据、也不转发」。修复后纯监视/转发工程必须照常工作。
#include "PvTest.h"

#include <chrono>
#include <thread>

#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/Project.h" // Tag / TagDataType
#include "base/packet/TcpLink.h"
#include "base/packet/UdpLink.h"
#include "base/packet/HexUtil.h"
#include "viewer/PollWorker.h"

using namespace pv;

namespace {

std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    std::string err;
    if (!packet::hexToBytes(s, v, err))
        std::printf("    [hex] 解析失败: %s\n", err.c_str());
    return v;
}

} // namespace

TEST_CASE("PollWorker：无标签工程（UDP 服务端 + TCP 客户端数据目的）仍收包并转发") {
    const int srcPort = 59380, platPort = 59381;

    // 平台侧：TCP 服务端监听（数据目的 TCP 客户端的连接目标）
    packet::TcpLink platform;
    std::string err;
    if (!platform.listen(platPort, err)) {
        std::printf("    [skip] 端口 %d 监听失败: %s\n", platPort, err.c_str());
        return;
    }

    // 工程设置：帧数据源 UDP 服务端（autoStart）+ 数据目的 TCP 客户端；无任何标签
    ProjectSettings ps;
    ps.frame.enabled = true;
    ps.frame.udp = true;
    ps.frame.localPort = srcPort;
    ps.frame.autoStart = true;
    ps.frame.framing.mode = packet::FrameMode::Tlv;
    ps.frame.sourceName = "采集";
    TagField f;
    f.name = "温度";
    f.tagId = 1; // TLV：T=01
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.address = 0;
    ps.frame.fields.push_back(f);
    FrameSinkSettings sk;
    sk.sourceName = "采集";
    sk.tcpClient = true; // TCP 客户端 → 连接平台
    sk.host = "127.0.0.1";
    sk.remotePort = platPort;
    ps.frame.sinks.push_back(sk);

    viewer::PollWorker worker;
    worker.start(ps, {}); // 无标签：仍须泵收包并转发（回归点）

    // 等 worker 完成数据源连接再发包（UDP 无缓存：绑定前的数据报会被丢弃）
    for (int i = 0; i < 40 && !worker.isConnected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(worker.isConnected());

    // 模拟设备：UDP 发 20 帧 TLV（T=01 L=0002 V=01F4），分 4 批批间 50ms
    packet::UdpLink device;
    REQUIRE(device.start(0, err));
    device.setRemote("127.0.0.1", srcPort);
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < 5; ++i)
            CHECK(device.send(hex("01 00 02 01 F4"), err));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 平台侧收转发字节流：20 帧 × 5B = 100B（上限 5s；TCP 流式按字节累计）
    size_t got = 0;
    std::deque<packet::TcpChunk> chunks;
    for (int i = 0; i < 100 && got < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        platform.drain(chunks);
        for (const auto& c : chunks) got += c.data.size();
        chunks.clear();
    }
    std::printf("    [info] 平台收到 %zu/100 字节，worker.lastError=\"%s\"\n", got,
                worker.lastError().c_str());
    CHECK(got == 100); // 全量转发，一字节不多不少

    // 报文监视（UI 侧帧日志）也应有数据
    std::deque<FrameDataSource::FrameLogEntry> log;
    worker.drainFrames(log);
    CHECK(!log.empty());

    worker.stop();
    device.stop();
}

TEST_CASE("PollWorker：有标签时 UDP 服务端收包刷新标签值") {
    const int srcPort = 59382;

    ProjectSettings ps;
    ps.frame.enabled = true;
    ps.frame.udp = true;
    ps.frame.localPort = srcPort;
    ps.frame.autoStart = true;
    ps.frame.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "温度";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.address = 0;
    ps.frame.fields.push_back(f);

    Tag t;
    t.name = "温度";
    t.address = 0;
    t.type = TagDataType::UInt16;

    viewer::PollWorker worker;
    worker.start(ps, {t});

    // 等 worker 完成数据源连接再发包（绑定前的数据报会被丢弃）
    for (int i = 0; i < 40 && !worker.isConnected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(worker.isConnected());

    packet::UdpLink device;
    std::string err;
    REQUIRE(device.start(0, err));
    device.setRemote("127.0.0.1", srcPort);
    CHECK(device.send(hex("01 00 02 01 F4"), err)); // 500

    bool ok = false;
    int64_t v = 0;
    for (int i = 0; i < 100 && !ok; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (const auto& r : worker.drainResults()) {
            if (r.ok && r.tag == "温度") {
                ok = true;
                if (auto* i64 = std::get_if<int64_t>(&r.value)) v = *i64;
                if (auto* d = std::get_if<double>(&r.value)) v = (int64_t)*d;
            }
        }
    }
    CHECK(ok);   // 标签值经 worker 正常刷新（收包泵 + 读结果队列）
    CHECK(v == 500);

    worker.stop();
    device.stop();
}
