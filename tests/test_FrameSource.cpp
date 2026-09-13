// M8 单测：帧数据源组件 — UDP 回环收帧 → 规约解析 → 标签值刷新
#include "SoftgTest.h"

#include <chrono>
#include <thread>

#include "base/data/frame/FrameDataSource.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/HexUtil.h"
#include "base/packet/SerialLink.h"
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
    f.address = 0; // 直接构造 settings 需显式槽位（自动序号仅组件解析路径）
    cfg.fields.push_back(f);
    f.name = "泵";
    f.tagId = 2;
    f.address = 1; // 直接构造 settings 需显式槽位
    f.offset = 0;
    f.type = packet::FieldType::U8;
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

    Tag t1 = makeTag("温度", 0, TagDataType::Float32, 0.1);
    Tag t2 = makeTag("泵", 1, TagDataType::Bool, 1.0);
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

    Tag t = makeTag("温度", 0, TagDataType::Float32, 1.0);
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    double eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(eng == 500.0); // 未被槽位 3 的帧覆盖

    sender.stop();
    src.disconnect();
}

TEST_CASE("帧数据源：数据源+协议组件 -> 工程级合成") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    // 协议配置组件：帧头+Length 拆帧 + 2 字段
    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "proto-1");
    proto.name = "设备协议A";
    proto.setProp("framingMode", std::string("帧头+Length"));
    proto.setProp("headerHex", std::string("EB 90"));
    proto.setProp("lenOffset", int64_t(3));
    proto.setProp("lenBytesHeader", int64_t(1));
    proto.setProp("bigEndianHeader", false);
    proto.setProp("lenIncludesAll", true);
    proto.setProp("fieldCount", int64_t(2));
    proto.setProp("f0.name", std::string("温度"));
    proto.setProp("f0.tagId", int64_t(1));
    proto.setProp("f0.offset", int64_t(4));
    proto.setProp("f0.type", std::string("f32"));
    proto.setProp("f0.bigEndian", false);
    proto.setProp("f1.name", std::string("计数"));
    proto.setProp("f1.tagId", int64_t(2));
    proto.setProp("f1.offset", int64_t(8));
    proto.setProp("f1.type", std::string("u32"));
    proto.setProp("f1.scale", 0.1);
    p.pages[0].components.push_back(proto);

    // 无数据源：回退工程设置（enabled=false）
    FrameSourceSettings none = frameSettingsFromProject(p);
    CHECK(!none.enabled);

    // 数据源组件（TCP + 关联协议）
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("transport", std::string("TCP"));
    ds.setProp("host", std::string("192.168.1.50"));
    ds.setProp("remotePort", int64_t(8888));
    ds.setProp("protocol", std::string("设备协议A"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    CHECK(!s.udp); // 传输来自数据源
    CHECK(s.host == "192.168.1.50");
    CHECK(s.remotePort == 8888);
    // 拆帧/字段来自协议组件
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
    CHECK(s.fields[0].address == 0); // 槽位=字段序号
    CHECK(s.fields[1].type == packet::FieldType::U32);
    CHECK(s.fields[1].address == 1);
    CHECK(s.fields[0].scale == 1.0); // 缺省不缩放
    CHECK(s.fields[1].scale == 0.1); // 属性存储的 scale

    // 协议名不存在：默认 TLV 无字段（仍可收帧监视）
    p.pages[0].components.back().setProp("protocol", std::string("不存在"));
    FrameSourceSettings miss = frameSettingsFromProject(p);
    CHECK(miss.enabled);
    CHECK(miss.framing.mode == packet::FrameMode::Tlv);
    CHECK(miss.fields.empty());

    // 默认数据源（未改属性）：UDP + 未关联协议
    Component fresh = ComponentRegistry::createComponent("DataSource", "ds-2");
    p.pages[0].components.clear();
    p.pages[0].components.push_back(fresh);
    FrameSourceSettings d = frameSettingsFromProject(p);
    CHECK(d.enabled);
    CHECK(d.udp);
    CHECK(d.framing.mode == packet::FrameMode::Tlv);
    CHECK(d.fields.empty());
}

TEST_CASE("规约字段 scale：组件属性解析 + 隐式标签升 Float32") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));
    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "proto-1");
    proto.name = "称重协议";
    proto.setProp("fieldCount", int64_t(2));
    proto.setProp("f0.name", std::string("重量"));
    proto.setProp("f0.tagId", int64_t(1));
    proto.setProp("f0.type", std::string("u16"));
    proto.setProp("f0.scale", 0.01);
    proto.setProp("f1.name", std::string("次数"));
    proto.setProp("f1.tagId", int64_t(2));
    proto.setProp("f1.type", std::string("u16"));
    p.pages[0].components.push_back(proto);

    packet::FramingConfig fr;
    std::vector<TagField> fields;
    protocolFramingFromComponent(proto, fr, fields);
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].scale == 0.01); // 属性存储的 scale
    CHECK(fields[1].scale == 1.0);  // 缺省不缩放

    // 隐式标签：整数字段带 scale → 工程值可能为小数，标签类型升 Float32
    Component gauge = ComponentRegistry::createComponent("Gauge", "g-1");
    gauge.setProp("bindField", std::string("称重协议/重量"));
    p.pages[0].components.push_back(gauge);
    synthesizeImplicitBindings(p);
    const Tag* t = p.tags.find("重量");
    REQUIRE(t != nullptr);
    CHECK(t->type == TagDataType::Float32);
    CHECK(t->scale == 1.0); // 换算在字段侧，标签不二次缩放
}

TEST_CASE("帧数据源：字段 scale 工程换算（协议侧）") {
    FrameSourceSettings cfg;
    cfg.udp = true;
    cfg.localPort = 59325;
    cfg.remotePort = 59325;
    cfg.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "温度";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.scale = 0.01; // 字段侧换算
    f.address = 0;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59325 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    CHECK(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59325);
    CHECK(sender.send(hex("01 00 02 01 F5"), err)); // 原始 501
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t = makeTag("温度", 0, TagDataType::Float32, 1.0); // 标签不再缩放
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    CHECK(results[0].ok);
    double eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(std::abs(eng - 5.01) < 1e-9); // 501 * 0.01
    CHECK(std::get_if<double>(&results[0].value) != nullptr); // 小数工程值保持 double

    sender.stop();
    src.disconnect();
}

TEST_CASE("帧数据源：串口传输的工程级合成") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("transport", std::string("串口"));
    ds.setProp("serialPort", std::string("COM7"));
    ds.setProp("baud", int64_t(115200));
    ds.setProp("dataBits", int64_t(8));
    ds.setProp("parity", std::string("偶"));
    ds.setProp("stopBits", int64_t(2));
    ds.setProp("protocol", std::string("设备协议A"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    CHECK(s.serial);       // 传输=串口
    CHECK(!s.udp);         // 不再落入「非 TCP 即 UDP」
    CHECK(s.serialPort == "COM7");
    CHECK(s.baud == 115200);
    CHECK(s.dataBits == 8);
    CHECK(s.parity == "偶");
    CHECK(s.stopBits == 2);

    // 默认串口参数（未改属性）
    Component fresh = ComponentRegistry::createComponent("DataSource", "ds-2");
    fresh.setProp("transport", std::string("串口"));
    p.pages[0].components.clear();
    p.pages[0].components.push_back(fresh);
    FrameSourceSettings d = frameSettingsFromProject(p);
    CHECK(d.serial);
    CHECK(d.serialPort == "COM1");
    CHECK(d.baud == 9600);
    CHECK(d.parity == "无");
    CHECK(d.stopBits == 1);
}

TEST_CASE("串口链路：不存在端口打开失败 + 关闭幂等") {
    packet::SerialLink ser;
    std::string err;
    CHECK(!ser.open("COM_不存在_", 9600, 8, "无", 1, err)); // 无此设备必然失败
    CHECK(!err.empty());
    CHECK(!ser.isOpen());
    ser.close(); // 关闭未打开的串口不得崩溃
    CHECK(!ser.isOpen());
    std::deque<packet::TcpChunk> chunks;
    ser.drain(chunks); // 未打开时 drain 为空
    CHECK(chunks.empty());
}

TEST_CASE("隐式绑定合成：组件 bindField -> 标签 + 数据绑定") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    // 协议配置：1 字段（槽位 1，f32）
    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "proto-1");
    proto.name = "温控协议";
    proto.setProp("fieldCount", int64_t(1));
    proto.setProp("f0.name", std::string("温度"));
    proto.setProp("f0.tagId", int64_t(1));
    proto.setProp("f0.offset", int64_t(0));
    proto.setProp("f0.type", std::string("f32"));
    p.pages[0].components.push_back(proto);

    // 仪表与文本组件直接绑定协议字段
    Component gauge = ComponentRegistry::createComponent("Gauge", "g-1");
    gauge.setProp("bindField", std::string("温控协议/温度"));
    p.pages[0].components.push_back(gauge);
    Component label = ComponentRegistry::createComponent("Label", "l-1");
    label.setProp("bindField", std::string("温控协议/温度"));
    p.pages[0].components.push_back(label);
    Component lamp = ComponentRegistry::createComponent("Lamp", "lp-1");
    lamp.setProp("bindField", std::string("温控协议/不存在字段"));
    p.pages[0].components.push_back(lamp);

    synthesizeImplicitBindings(p);

    // 隐式标签：1 个（槽位 1，float32，名字 = 字段名）
    CHECK(p.tags.all().size() == 1);
    const Tag* t = p.tags.find("温度");
    REQUIRE(t != nullptr);
    CHECK(t->address == 0);
    CHECK(t->type == TagDataType::Float32);
    CHECK(t->scale == 1.0);

    // 绑定：gauge->value、label->text；lamp 字段不存在被跳过
    CHECK(p.associations.size() == 2);
    const DataBinding* bg = nullptr;
    const DataBinding* bl = nullptr;
    for (const auto& a : p.associations) {
        if (auto* b = std::get_if<DataBinding>(&a)) {
            if (b->component == "g-1") bg = b;
            if (b->component == "l-1") bl = b;
        }
    }
    REQUIRE(bg != nullptr);
    CHECK(bg->property == "value");
    CHECK(bg->tag == "温度");
    REQUIRE(bl != nullptr);
    CHECK(bl->property == "text");

    // 幂等：重复合成不重复创建
    synthesizeImplicitBindings(p);
    CHECK(p.tags.all().size() == 1);
    CHECK(p.associations.size() == 2);

    // 默认绑定属性映射
    CHECK(std::string(defaultBindableProperty(gauge)) == "value");
    CHECK(std::string(defaultBindableProperty(label)) == "text");
    CHECK(std::string(defaultBindableProperty(lamp)) == "isOn");

    // 历史格式容忍："协议名 / 字段名"（斜杠带空格）同样可解析合成
    p.pages[0].components[1].setProp("bindField", std::string(" 温控协议 / 温度 "));
    p.associations.clear();
    synthesizeImplicitBindings(p);
    bool regen = false;
    for (const auto& a : p.associations)
        if (auto* b = std::get_if<DataBinding>(&a); b && b->component == "g-1")
            regen = b->tag == "温度";
    CHECK(regen);
}

TEST_CASE("帧数据源：帧头+Length 字段偏移相对负载") {
    FrameSourceSettings cfg;
    cfg.udp = true;
    cfg.localPort = 59324;
    cfg.remotePort = 59324;
    cfg.framing.mode = packet::FrameMode::HeaderLength;
    cfg.framing.header = hex("AA 55");
    cfg.framing.lenOffset = 2;
    cfg.framing.lenBytesHeader = 2;
    cfg.framing.bigEndianHeader = true;
    cfg.framing.lenIncludesAll = false; // length 只计负载

    // 两个字段 t1/t2：u8，偏移 0/1（相对负载）
    TagField f;
    f.name = "t1";
    f.address = 0; // 直接构造 settings 需显式槽位
    f.offset = 0;
    f.type = packet::FieldType::U8;
    cfg.fields.push_back(f);
    f.name = "t2";
    f.address = 1; // 直接构造 settings 需显式槽位
    f.offset = 1;
    f.type = packet::FieldType::U8;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59324 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    CHECK(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59324);
    // 帧 = AA 55 | 00 02 | 03 04（负载 2 字节）
    CHECK(sender.send(hex("AA 55 00 02 03 04"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t1 = makeTag("t1", 0, TagDataType::UInt16, 1.0);
    Tag t2 = makeTag("t2", 1, TagDataType::UInt16, 1.0);
    std::vector<const Tag*> tags = {&t1, &t2};
    auto results = src.readTags(tags);
    CHECK(results[0].ok);
    int64_t v1 = std::get_if<int64_t>(&results[0].value) ? *std::get_if<int64_t>(&results[0].value) : -1;
    int64_t v2 = std::get_if<int64_t>(&results[1].value) ? *std::get_if<int64_t>(&results[1].value) : -1;
    CHECK(v1 == 0x03); // 偏移 0 -> 负载首字节 03（而非帧头 AA）
    CHECK(v2 == 0x04);

    // 帧头不符的报文整体丢弃（不更新值）
    CHECK(sender.send(hex("BB 66 00 02 09 09"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    results = src.readTags(tags);
    v1 = std::get_if<int64_t>(&results[0].value) ? *std::get_if<int64_t>(&results[0].value) : -1;
    CHECK(v1 == 0x03); // 未被污染

    sender.stop();
    src.disconnect();
}

TEST_CASE("帧数据源：断开后标签质量为不可用") {
    FrameSourceSettings cfg;
    cfg.udp = true;
    cfg.localPort = 59322;
    cfg.remotePort = 59322;
    TagField f;
    f.name = "温度";
    f.type = packet::FieldType::U16;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    Tag t = makeTag("温度", 0, TagDataType::Float32, 1.0);
    std::vector<const Tag*> tags = {&t};

    // 未连接（未 start）：直接 readTags 应返回等待数据
    auto results = src.readTags(tags);
    CHECK(!results[0].ok);
}

TEST_CASE("帧数据源：UDP 角色 -> 客户端/服务端设置合成") {
    auto makeDs = [](const char* transport, const char* role) {
        Project p;
        Page pg;
        pg.id = "page-1";
        Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
        ds.setProp("transport", std::string(transport));
        if (role) ds.setProp("udpRole", std::string(role));
        ds.setProp("host", std::string("10.0.0.5"));
        ds.setProp("remotePort", int64_t(5000));
        pg.components.push_back(std::move(ds));
        p.pages.push_back(std::move(pg));
        return p;
    };

    Project pc = makeDs("UDP", "客户端");
    FrameSourceSettings sc = frameSettingsFromProject(pc);
    CHECK(sc.enabled);
    CHECK(sc.udp);
    CHECK(sc.udpClient);
    CHECK(sc.remotePort == 5000);

    Project ps = makeDs("UDP", "服务端");
    CHECK(!frameSettingsFromProject(ps).udpClient);

    Project pd = makeDs("UDP", nullptr); // 缺省按服务端（向后兼容旧工程）
    CHECK(!frameSettingsFromProject(pd).udpClient);

    Project pt = makeDs("TCP", "客户端"); // TCP 忽略 UDP 角色
    CHECK(!frameSettingsFromProject(pt).udpClient);
}

TEST_CASE("UdpLink 客户端：connect 远端收包") {
    const int serverPort = 59330;
    packet::UdpLink server;
    std::string err;
    if (!server.start(serverPort, err)) {
        std::printf("    [skip] 端口 %d 绑定失败: %s\n", serverPort, err.c_str());
        return;
    }
    packet::UdpLink client;
    CHECK(client.startClient("127.0.0.1", serverPort, err));
    CHECK(client.isRunning());

    // 客户端先发探测帧，服务端据此获知客户端临时端口
    CHECK(client.send(hex("AA 55 00 02 03 04"), err));
    std::deque<packet::UdpPacket> atServer;
    for (int i = 0; i < 20 && atServer.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.drain(atServer);
    }
    REQUIRE(!atServer.empty());
    std::string from = atServer.front().from; // "ip:port"
    size_t colon = from.rfind(':');
    REQUIRE(colon != std::string::npos);
    server.setRemote(from.substr(0, colon), std::stoi(from.substr(colon + 1)));

    // 服务端回发 → 客户端仅收该对端，应收到
    CHECK(server.send(hex("11 22 33"), err));
    std::deque<packet::UdpPacket> atClient;
    for (int i = 0; i < 20 && atClient.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        client.drain(atClient);
    }
    REQUIRE(!atClient.empty());
    CHECK(atClient.front().data == hex("11 22 33"));

    client.stop();
    server.stop();
}

TEST_CASE("帧数据源：UDP 客户端 connect 远端冒烟") {
    const int serverPort = 59331;
    packet::UdpLink server;
    std::string err;
    if (!server.start(serverPort, err)) {
        std::printf("    [skip] 端口 %d 绑定失败: %s\n", serverPort, err.c_str());
        return;
    }
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = true;
    cfg.udpClient = true;
    cfg.host = "127.0.0.1";
    cfg.remotePort = serverPort;

    FrameDataSource src(cfg);
    CHECK(src.connect(err));
    CHECK(src.isConnected());
    src.disconnect();
    server.stop();
}

TEST_CASE("帧数据源：TCP 角色 -> 客户端/服务端设置合成") {
    auto makeDs = [](const char* role) {
        Project p;
        Page pg;
        pg.id = "page-1";
        Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
        ds.setProp("transport", std::string("TCP"));
        if (role) ds.setProp("tcpRole", std::string(role));
        ds.setProp("host", std::string("10.0.0.6"));
        ds.setProp("remotePort", int64_t(6000));
        ds.setProp("localPort", int64_t(6001));
        pg.components.push_back(std::move(ds));
        p.pages.push_back(std::move(pg));
        return p;
    };

    FrameSourceSettings sc = frameSettingsFromProject(makeDs("客户端"));
    CHECK(sc.enabled);
    CHECK(!sc.udp);
    CHECK(sc.tcpClient); // 客户端=连接远端
    CHECK(!sc.udpClient); // TCP 不影响 UDP 角色

    FrameSourceSettings ss = frameSettingsFromProject(makeDs("服务端"));
    CHECK(!ss.tcpClient); // 服务端=监听本地
    CHECK(ss.localPort == 6001);

    CHECK(frameSettingsFromProject(makeDs(nullptr)).tcpClient); // 缺省=客户端（向后兼容）
}

TEST_CASE("TcpLink 服务端：监听接入收字节并可重连") {
    const int port = 59340;
    packet::TcpLink server;
    std::string err;
    if (!server.listen(port, err)) {
        std::printf("    [skip] 端口 %d 监听失败: %s\n", port, err.c_str());
        return;
    }
    CHECK(server.isConnected()); // 监听中即视为已接入

    packet::TcpLink client;
    CHECK(client.connect("127.0.0.1", port, err));
    CHECK(client.send(hex("DE AD BE EF"), err));
    std::deque<packet::TcpChunk> in;
    for (int i = 0; i < 20 && in.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.drain(in);
    }
    REQUIRE(!in.empty());
    CHECK(in.front().data == hex("DE AD BE EF"));

    // 断开后服务端回到监听，仍可再次接入
    client.disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(server.isConnected());

    packet::TcpLink client2;
    CHECK(client2.connect("127.0.0.1", port, err));
    CHECK(client2.send(hex("01 02"), err));
    std::deque<packet::TcpChunk> in2;
    for (int i = 0; i < 20 && in2.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.drain(in2);
    }
    REQUIRE(!in2.empty());
    CHECK(in2.front().data == hex("01 02"));

    client2.disconnect();
    server.disconnect();
}

TEST_CASE("帧数据源：TCP 服务端接入 → 解析 → 标签值") {
    const int port = 59341;
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = false;
    cfg.tcpClient = false; // 服务端：监听本地端口
    cfg.localPort = port;
    cfg.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "温度";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.bigEndian = true;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    CHECK(src.connect(err));
    CHECK(src.isConnected());

    // 设备主动连接并发送 TLV 帧 T=01 L=0002 V=01F4(500)
    packet::TcpLink client;
    CHECK(client.connect("127.0.0.1", port, err));
    CHECK(client.send(hex("01 00 02 01 F4"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t = makeTag("温度", 0, TagDataType::Float32, 0.1);
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    REQUIRE(results.size() == 1);
    CHECK(results[0].ok);
    double eng = 0;
    if (auto* d = std::get_if<double>(&results[0].value)) eng = *d;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) eng = (double)*i;
    CHECK(eng == 50.0); // 500 * 0.1

    client.disconnect();
    src.disconnect();
    CHECK(!src.isConnected());
}

TEST_CASE("帧数据源：布尔/字符串/枚举字段 → 标签值") {
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = true;
    cfg.localPort = 59350;
    cfg.remotePort = 59350;
    cfg.framing.mode = packet::FrameMode::Tlv;

    TagField f;
    f.name = "开关";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::Bool;
    f.bytes = 1;
    f.address = 0;
    cfg.fields.push_back(f);
    f.name = "状态";
    f.tagId = 2;
    f.offset = 0;
    f.type = packet::FieldType::Enum;
    f.bytes = 1;
    f.address = 1;
    f.enums = {{0, "停止"}, {1, "运行"}};
    cfg.fields.push_back(f);
    f.enums.clear();
    f.name = "名称";
    f.tagId = 3;
    f.offset = 0;
    f.type = packet::FieldType::String;
    f.bytes = 4;
    f.address = 2;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    CHECK(src.connect(err));
    CHECK(src.isConnected());

    packet::UdpLink sender;
    CHECK(sender.start(0, err)); // 系统分配临时端口
    sender.setRemote("127.0.0.1", 59350);
    CHECK(sender.send(hex("01 00 01 01"), err));             // T=01 V=01 → 布尔真
    CHECK(sender.send(hex("02 00 01 01"), err));             // T=02 V=01 → 枚举「运行」
    CHECK(sender.send(hex("03 00 04 41 42 00 00"), err));    // T=03 V="AB\0\0"
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t0 = makeTag("开关", 0, TagDataType::Bool, 1.0);
    Tag t1 = makeTag("状态", 1, TagDataType::String, 1.0);
    Tag t2 = makeTag("名称", 2, TagDataType::String, 1.0);
    std::vector<const Tag*> tags = {&t0, &t1, &t2};
    auto results = src.readTags(tags);
    REQUIRE(results.size() == 3);
    CHECK(results[0].ok);
    CHECK(std::get<bool>(results[0].value) == true);
    CHECK(results[1].ok);
    CHECK(std::get<std::string>(results[1].value) == "运行");
    CHECK(results[2].ok);
    CHECK(std::get<std::string>(results[2].value) == "AB"); // 去尾 NUL

    sender.stop();
    src.disconnect();
}

TEST_CASE("隐式绑定合成：布尔/枚举字段 → 标签类型") {
    Project p;
    Page pg;
    pg.id = "page-1";
    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "proto-1");
    proto.name = "协议A";
    proto.setProp("fieldCount", int64_t(3));
    proto.setProp("f0.name", std::string("开关"));
    proto.setProp("f0.tagId", int64_t(1));
    proto.setProp("f0.offset", int64_t(0));
    proto.setProp("f0.type", std::string("bool"));
    proto.setProp("f1.name", std::string("状态"));
    proto.setProp("f1.tagId", int64_t(2));
    proto.setProp("f1.offset", int64_t(1));
    proto.setProp("f1.type", std::string("enum"));
    proto.setProp("f1.len", int64_t(1));
    proto.setProp("f1.enumCount", int64_t(2));
    proto.setProp("f1.e0.v", int64_t(0));
    proto.setProp("f1.e0.n", std::string("停"));
    proto.setProp("f1.e1.v", int64_t(1));
    proto.setProp("f1.e1.n", std::string("行"));
    proto.setProp("f2.name", std::string("名称"));
    proto.setProp("f2.tagId", int64_t(3));
    proto.setProp("f2.offset", int64_t(2));
    proto.setProp("f2.type", std::string("string"));
    proto.setProp("f2.len", int64_t(4));
    pg.components.push_back(proto);

    Component lamp = ComponentRegistry::createComponent("Lamp", "lp-1");
    lamp.setProp("bindField", std::string("协议A/开关"));
    pg.components.push_back(lamp);
    Component label = ComponentRegistry::createComponent("Label", "l-1");
    label.setProp("bindField", std::string("协议A/状态"));
    pg.components.push_back(label);
    p.pages.push_back(std::move(pg));

    synthesizeImplicitBindings(p);

    const Tag* tb = p.tags.find("开关");
    REQUIRE(tb != nullptr);
    CHECK(tb->type == TagDataType::Bool); // 布尔字段 → 布尔标签
    const Tag* ts = p.tags.find("状态");
    REQUIRE(ts != nullptr);
    CHECK(ts->type == TagDataType::String); // 枚举 → 字符串标签（名称文本）
}
