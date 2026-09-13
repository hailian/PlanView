// M8 单测：帧数据源组件 — UDP 回环收帧 → 规约解析 → 标签值刷新
#include "SoftgTest.h"

#include <chrono>
#include <thread>

#include "base/data/frame/FrameDataSource.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/HexUtil.h"
#include "base/packet/UdpLink.h"

using namespace softg;

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    std::string err;
    if (!packet::hexToBytes(s, v, err))
        std::printf("    [hex] 解析失败: %s\n", err.c_str());
    return v;
}

static Tag makeTag(const char* name, int address, TagDataType type, double scale) {
    Tag t;
    t.name = name;
    t.address = address;
    t.type = type;
    t.scale = scale;
    return t;
}

TEST_CASE("帧数据源：UDP 回环 → 解析 → 标签值") {
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = true;
    cfg.host = "127.0.0.1";
    cfg.remotePort = 59321;
    cfg.localPort = 59321;
    cfg.framing.mode = packet::FrameMode::Tlv; // UDP 实际不用拆帧器，配置不影响

    TagField f;
    f.name = "温度";
    f.tagId = 1;  // TLV：匹配 T=01 的帧
    f.offset = 0; // 偏移相对该帧负载 V
    f.type = packet::FieldType::U16;
    f.bigEndian = true;
    f.address = 1; // 对应标签槽位 1
    cfg.fields.push_back(f);
    f.name = "泵";
    f.tagId = 2;
    f.offset = 0;
    f.type = packet::FieldType::U8;
    f.address = 2;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59321 绑定失败: %s\n", err.c_str());
        return;
    }
    CHECK(src.isConnected());
    CHECK(!src.supportsWrite());

    // 模拟设备：向同端口发 TLV 帧 T=01 L=0002 V=01F4(500)；T=02 L=0001 V=01
    packet::UdpLink sender;
    CHECK(sender.start(0, err)); // 系统分配临时端口
    sender.setRemote("127.0.0.1", 59321);
    CHECK(sender.send(hex("01 00 02 01 F4"), err));
    CHECK(sender.send(hex("02 00 01 01"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t1 = makeTag("温度", 1, TagDataType::Float32, 0.1);
    Tag t2 = makeTag("泵", 2, TagDataType::Bool, 1.0);
    Tag t3 = makeTag("无数据", 9, TagDataType::UInt16, 1.0);
    std::vector<const Tag*> tags = {&t1, &t2, &t3};
    auto results = src.readTags(tags);
    CHECK(results.size() == 3);

    CHECK(results[0].ok);
    CHECK(results[0].quality == TagQuality::Good);
    double eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(eng == 50.0); // 500 * 0.1

    CHECK(results[1].ok);
    CHECK(std::get<bool>(results[1].value) == true);

    CHECK(!results[2].ok); // 槽位 9 无字段映射
    CHECK(results[2].quality == TagQuality::Bad);

    // 帧日志已收到 2 帧
    std::deque<FrameDataSource::FrameLogEntry> log;
    src.drainFrameLog(log);
    CHECK(log.size() == 2);
    CHECK(packet::bytesToHex(log.front().data) == "01 00 02 01 F4");

    // 再发一帧：缓存刷新为最新值
    CHECK(sender.send(hex("01 00 02 00 C8"), err)); // 200 * 0.1 = 20
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    results = src.readTags(tags);
    eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(eng == 20.0);

    sender.stop();
    src.disconnect();
    CHECK(!src.isConnected());
}

TEST_CASE("帧数据源：TLV 跨槽位隔离") {
    FrameSourceSettings cfg;
    cfg.udp = true;
    cfg.localPort = 59323;
    cfg.remotePort = 59323;
    cfg.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "温度";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.address = 1;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59323 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    CHECK(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59323);

    CHECK(sender.send(hex("01 00 02 01 F4"), err)); // 槽位1: 500
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    // 其他槽位的帧（f32 14.6 → 前两字节 41 69）不得覆盖槽位 1
    CHECK(sender.send(hex("03 00 04 41 69 99 9A"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    Tag t = makeTag("温度", 1, TagDataType::Float32, 1.0);
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    double eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(eng == 500.0); // 未被槽位 3 的帧覆盖

    sender.stop();
    src.disconnect();
}

TEST_CASE("帧数据源：数据源组件属性 -> 设置解析") {
    // 注册表创建（等同从组件面板拖入的初始状态）
    Component c = ComponentRegistry::createComponent("DataSource", "ds-1");
    CHECK(c.typeId == "DataSource");

    // 按用户在属性面板/规约字段区的编辑结果设置属性
    c.setProp("transport", std::string("TCP"));
    c.setProp("host", std::string("192.168.1.50"));
    c.setProp("remotePort", int64_t(8888));
    c.setProp("framingMode", std::string("帧头+Length"));
    c.setProp("headerHex", std::string("EB 90"));
    c.setProp("lenOffset", int64_t(3));
    c.setProp("lenBytesHeader", int64_t(1));
    c.setProp("bigEndianHeader", false);
    c.setProp("lenIncludesAll", true);
    c.setProp("fieldCount", int64_t(2));
    c.setProp("f0.name", std::string("温度"));
    c.setProp("f0.tagId", int64_t(1));
    c.setProp("f0.offset", int64_t(4));
    c.setProp("f0.type", std::string("f32"));
    c.setProp("f0.bigEndian", false);
    c.setProp("f0.address", int64_t(7));
    c.setProp("f1.name", std::string("计数"));
    c.setProp("f1.tagId", int64_t(2));
    c.setProp("f1.offset", int64_t(8));
    c.setProp("f1.type", std::string("u32"));
    c.setProp("f1.address", int64_t(9));

    FrameSourceSettings s = frameSettingsFromComponent(c);
    CHECK(s.enabled);
    CHECK(!s.udp);
    CHECK(s.host == "192.168.1.50");
    CHECK(s.remotePort == 8888);
    CHECK(s.framing.mode == packet::FrameMode::HeaderLength);
    CHECK(packet::bytesToHex(s.framing.header) == "EB 90");
    CHECK(s.framing.lenOffset == 3);
    CHECK(s.framing.lenBytesHeader == 1);
    CHECK(!s.framing.bigEndianHeader);
    CHECK(s.framing.lenIncludesAll);
    CHECK(s.fields.size() == 2);
    CHECK(s.fields[0].name == "温度");
    CHECK(s.fields[0].type == packet::FieldType::F32);
    CHECK(s.fields[0].bytes == 4);
    CHECK(s.fields[0].offset == 4);
    CHECK(s.fields[0].address == 7);
    CHECK(s.fields[1].type == packet::FieldType::U32);
    CHECK(s.fields[1].address == 9);

    // 默认（未改动任何属性）：UDP + TLV，无字段
    Component fresh = ComponentRegistry::createComponent("DataSource", "ds-2");
    FrameSourceSettings d = frameSettingsFromComponent(fresh);
    CHECK(d.enabled);
    CHECK(d.udp);
    CHECK(d.framing.mode == packet::FrameMode::Tlv);
    CHECK(d.fields.empty());
}

TEST_CASE("帧数据源：断开后标签质量为不可用") {
    FrameSourceSettings cfg;
    cfg.udp = true;
    cfg.localPort = 59322;
    cfg.remotePort = 59322;
    TagField f;
    f.name = "温度";
    f.type = packet::FieldType::U16;
    f.address = 0;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    Tag t = makeTag("温度", 0, TagDataType::Float32, 1.0);
    std::vector<const Tag*> tags = {&t};

    // 未连接（未 start）：直接 readTags 应返回等待数据
    auto results = src.readTags(tags);
    CHECK(!results[0].ok);
}
