// M8 单测：帧数据源组件 — UDP 回环收帧 → 规约解析 → 标签值刷新
#include "PvTest.h"

#include <chrono>
#include <thread>

#include "base/data/frame/FrameDataSource.h"
#include "base/data/frame/TestFrameGen.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/HexUtil.h"
#include "base/packet/SerialLink.h"
#include "base/packet/UdpLink.h"

using namespace pv;

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

    // 运行统计：末帧时间非空；两帧均命中字段 -> 计数 2
    CHECK(!src.lastFrameTimeText().empty());
    CHECK(src.matchedFrameCount() == 2);

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

TEST_CASE("协议组：数据源关联组 -> 组内字段合并（拆帧取首个）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    // 协议 A：TLV u16 槽位 1；协议 B：TLV f32 槽位 2
    Component pa = ComponentRegistry::createComponent("ProtocolConfig", "pa");
    pa.name = "协议A";
    pa.setProp("fieldCount", int64_t(1));
    pa.setProp("f0.name", std::string("温度"));
    pa.setProp("f0.tagId", int64_t(1));
    pa.setProp("f0.type", std::string("u16"));
    p.pages[0].components.push_back(pa);
    Component pb = ComponentRegistry::createComponent("ProtocolConfig", "pb");
    pb.name = "协议B";
    pb.setProp("fieldCount", int64_t(1));
    pb.setProp("f0.name", std::string("液位"));
    pb.setProp("f0.tagId", int64_t(2));
    pb.setProp("f0.type", std::string("f32"));
    p.pages[0].components.push_back(pb);

    // 协议组：成员 = 协议A、协议B
    Component grp = ComponentRegistry::createComponent("ProtocolGroup", "grp");
    grp.name = "泵站组";
    grp.setProp("protoCount", int64_t(2));
    grp.setProp("p0.name", std::string("协议A"));
    grp.setProp("p1.name", std::string("协议B"));
    p.pages[0].components.push_back(grp);

    // 数据源：关联协议组
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
    ds.setProp("group", std::string("泵站组"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    REQUIRE(s.fields.size() == 2);        // 组内字段合并
    CHECK(s.fields[0].name == "温度");
    CHECK(s.fields[0].tagId == 1);
    CHECK(s.fields[1].name == "液位");
    CHECK(s.fields[1].tagId == 2);
    CHECK(s.fields[0].address == 0);      // 隐式标签槽位按合并后序号
    CHECK(s.fields[1].address == 1);
    CHECK(s.framing.mode == packet::FrameMode::Tlv); // 拆帧取首个成员（TLV）

    // 组无效且同时关联了单协议：回退单协议（二选一语义）
    Component ds2 = p.pages[0].components.back();
    ds2.setProp("group", std::string("不存在"));
    ds2.setProp("protocol", std::string("协议A"));
    p.pages[0].components.back() = ds2;
    FrameSourceSettings fb = frameSettingsFromProject(p);
    REQUIRE(fb.fields.size() == 1);
    CHECK(fb.fields[0].name == "温度"); // 用的是协议A而非组

    // 组有效时优先于同时设置的单协议
    p.pages[0].components.back().setProp("group", std::string("泵站组"));
    FrameSourceSettings both = frameSettingsFromProject(p);
    REQUIRE(both.fields.size() == 2); // 组内两协议合并（单协议被组覆盖）
}

TEST_CASE("协议组多帧头：AA55 帧只驱动 AA55 协议字段，AA56 帧只驱动 AA56 协议") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    // 协议A：帧头 AA 55，负载 2 字节 u16
    Component pa = ComponentRegistry::createComponent("ProtocolConfig", "pa");
    pa.name = "协议A";
    pa.setProp("framingMode", std::string("帧头+Length"));
    pa.setProp("headerHex", std::string("AA 55"));
    pa.setProp("fieldCount", int64_t(1));
    pa.setProp("f0.name", std::string("阀位"));
    pa.setProp("f0.offset", int64_t(0));
    pa.setProp("f0.type", std::string("u16"));
    p.pages[0].components.push_back(pa);
    // 协议B：帧头 AA 56，负载 2 字节 u16
    Component pb = ComponentRegistry::createComponent("ProtocolConfig", "pb");
    pb.name = "协议B";
    pb.setProp("framingMode", std::string("帧头+Length"));
    pb.setProp("headerHex", std::string("AA 56"));
    pb.setProp("fieldCount", int64_t(1));
    pb.setProp("f0.name", std::string("转速"));
    pb.setProp("f0.offset", int64_t(0));
    pb.setProp("f0.type", std::string("u16"));
    p.pages[0].components.push_back(pb);

    Component grp = ComponentRegistry::createComponent("ProtocolGroup", "grp");
    grp.name = "双帧头组";
    grp.setProp("protoCount", int64_t(2));
    grp.setProp("p0.name", std::string("协议A"));
    grp.setProp("p1.name", std::string("协议B"));
    p.pages[0].components.push_back(grp);

    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
    ds.setProp("group", std::string("双帧头组"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings st = frameSettingsFromProject(p);
    CHECK(st.enabled);
    REQUIRE(st.fields.size() == 2);
    CHECK(st.framings.size() == 2);                 // 两条不同帧头的配置
    CHECK(st.fields[0].framingIndex == 0);          // 阀位 -> AA55
    CHECK(st.fields[1].framingIndex == 1);          // 转速 -> AA56
    std::vector<uint8_t> h55{0xAA, 0x55}, h56{0xAA, 0x56};
    CHECK(st.framings[0].header == h55);
    CHECK(st.framings[1].header == h56);

    // UDP 回环：AA55 帧发 0x0064(100)，AA56 帧发 0x0100(256)
    // 帧格式：帧头(2) + len(2, 大端, 负载长) + 负载(2)
    FrameSourceSettings cfg = st; // 直接用合成配置，仅改传输
    cfg.udp = true;
    cfg.localPort = 59326;
    cfg.remotePort = 59326;
    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59326 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    CHECK(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59326);
    CHECK(sender.send(hex("AA 55 00 02 00 64"), err)); // AA55: 阀位=100
    CHECK(sender.send(hex("AA 56 00 02 01 00"), err)); // AA56: 转速=256
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Tag t1 = makeTag("阀位", st.fields[0].address, TagDataType::UInt16, 1.0);
    Tag t2 = makeTag("转速", st.fields[1].address, TagDataType::UInt16, 1.0);
    std::vector<const Tag*> tags = {&t1, &t2};
    auto results = src.readTags(tags);
    int64_t v1 = 0, v2 = 0;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) v1 = *i;
    if (auto* d = std::get_if<double>(&results[0].value)) v1 = (int64_t)*d;
    if (auto* i = std::get_if<int64_t>(&results[1].value)) v2 = *i;
    if (auto* d = std::get_if<double>(&results[1].value)) v2 = (int64_t)*d;
    CHECK(results[0].ok);
    CHECK(results[1].ok);
    CHECK(v1 == 100); // AA55 帧驱动 阀位
    CHECK(v2 == 256); // AA56 帧驱动 转速（不再被 AA55 帧污染）

    // 隐式标签按运行时槽位合成：协议A/协议B 的字段0 不得共享同一标签
    //（回归：此前两协议字段0都按协议内序号=0 合成，AA55 帧两张卡都显示阀位值）
    {
        Project p2 = p; // 结构拷贝
        Component la = ComponentRegistry::createComponent("Label", "la");
        la.setProp("bindField", std::string("协议A/阀位"));
        p2.pages[0].components.push_back(la);
        Component lb = ComponentRegistry::createComponent("Label", "lb");
        lb.setProp("bindField", std::string("协议B/转速"));
        p2.pages[0].components.push_back(lb);
        synthesizeImplicitBindings(p2);
        const Tag* ta = p2.tags.find("阀位");
        const Tag* tb = p2.tags.find("转速");
        REQUIRE(ta != nullptr);
        REQUIRE(tb != nullptr);
        CHECK(ta->address == 0);
        CHECK(tb->address == 1);      // 组内重排槽位，不再是两协议共用的 0
        CHECK(ta != tb);
        // 两张卡绑定不同标签（此前转速卡会复用槽位 0 的「阀位」标签）
        int distinct = 0;
        for (const auto& a : p2.associations)
            if (auto* b = std::get_if<DataBinding>(&a); b && b->tag == "转速") ++distinct;
        CHECK(distinct == 1);
    }

    // 帧计数按帧头归属分组：AA55 协议只计 AA55 帧，AA56 协议只计 AA56 帧
    {
        auto byIdx = src.matchedFrameCountByIndex();
        CHECK(byIdx[0] == 1); // AA55：已发 1 帧
        CHECK(byIdx[1] == 1); // AA56：已发 1 帧
    }

    // 再发 AA55 帧改变阀位，转速必须保持不变（隔离）
    CHECK(sender.send(hex("AA 55 00 02 00 C8"), err)); // 阀位=200
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    results = src.readTags(tags);
    v1 = 0; v2 = 0;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) v1 = *i;
    if (auto* d = std::get_if<double>(&results[0].value)) v1 = (int64_t)*d;
    if (auto* i = std::get_if<int64_t>(&results[1].value)) v2 = *i;
    if (auto* d = std::get_if<double>(&results[1].value)) v2 = (int64_t)*d;
    CHECK(v1 == 200);
    CHECK(v2 == 256); // 隔离：AA55 帧不影响 AA56 协议字段

    // 帧计数隔离：AA55 计 2，AA56 仍为 1
    {
        auto byIdx = src.matchedFrameCountByIndex();
        CHECK(byIdx[0] == 2);
        CHECK(byIdx[1] == 1); // AA56 协议卡计数不被 AA55 帧推动
    }

    sender.stop();
    src.disconnect();
}

TEST_CASE("测试帧生成：TLV 逐字段成帧且可拆回") {
    packet::FramingConfig fr; // 默认 TLV：T1B L2B 大端
    std::vector<TagField> fields;
    TagField a;
    a.name = "温度"; a.tagId = 1; a.offset = 0; a.type = packet::FieldType::U16; a.bytes = 2;
    fields.push_back(a);
    TagField b;
    b.name = "状态"; b.tagId = 2; b.offset = 2; b.type = packet::FieldType::Enum; b.bytes = 1;
    b.enums = {{0, "停止"}, {1, "运行"}};
    fields.push_back(b);

    auto frames = generateTestFrames(fr, fields, 3, 42);
    REQUIRE(frames.size() == 6); // 每字段一帧 x 3 组

    // 全部帧可拆回且结构对称：tagId/负载长度匹配 offset+bytes
    packet::FrameSplitter sp(fr);
    std::vector<std::vector<uint8_t>> out;
    for (const auto& f : frames) {
        sp.feed(f.data(), f.size(), out);
    }
    REQUIRE(out.size() == 6);
    for (const auto& f : out) {
        int64_t tagId = -1;
        const uint8_t* payload = nullptr;
        int payloadLen = 0;
        REQUIRE(packet::decodeFrameOnce(fr, f, tagId, payload, payloadLen));
        bool known = false;
        for (const auto& tf : fields)
            if (tf.tagId == tagId) {
                CHECK(payloadLen >= tf.offset + tf.bytes);
                known = true;
            }
        CHECK(known);
    }
    // 枚举帧负载末字节必是映射表中的编码值
    for (size_t i = 1; i < frames.size(); i += 2) {
        uint8_t v = frames[i].back();
        CHECK((v == 0 || v == 1));
    }
}

TEST_CASE("测试帧生成：帧头+Length 一帧含全部字段") {
    packet::FramingConfig fr;
    fr.mode = packet::FrameMode::HeaderLength;
    std::string hexErr;
    packet::hexToBytes("AA 55", fr.header, hexErr);
    fr.lenOffset = 2;
    fr.lenBytesHeader = 2;
    std::vector<TagField> fields;
    TagField a;
    a.name = "温度"; a.tagId = 0; a.offset = 0; a.type = packet::FieldType::F32; a.bytes = 4;
    fields.push_back(a);
    TagField b;
    b.name = "计数"; b.tagId = 0; a.offset = 4;
    b.offset = 4; b.type = packet::FieldType::U32; b.bytes = 4;
    fields.push_back(b);

    auto frames = generateTestFrames(fr, fields, 4, 7);
    REQUIRE(frames.size() == 4);
    for (const auto& f : frames) {
        CHECK(f.size() == 4 + 8);            // 帧头2 + length2 + 负载8
        CHECK(f[0] == 0xAA && f[1] == 0x55); // 帧头
        uint16_t len = (uint16_t)((f[2] << 8) | f[3]);
        CHECK(len == 8);                     // 大端 length = 负载长
    }
    // 喂拆帧器全部成帧
    packet::FrameSplitter sp(fr);
    std::vector<std::vector<uint8_t>> out;
    for (const auto& f : frames) sp.feed(f.data(), f.size(), out);
    CHECK(out.size() == 4);
}

TEST_CASE("监听数据源：三元组过滤（反向命中，未命中丢弃）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
    ds.setProp("transport", std::string("监听"));
    ds.setProp("listenIp", std::string("127.0.0.1"));
    ds.setProp("listenPort", int64_t(59342));
    ds.setProp("listenProto", std::string("UDP"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings st = frameSettingsFromProject(p);
    CHECK(st.enabled);
    CHECK(st.listen);
    CHECK(st.listenIp == "127.0.0.1");
    CHECK(st.listenPort == 59342);
    CHECK(!st.listenTcp);

    // 链路层过滤单测：绑定 59343，命中源 = (127.0.0.1, 59342)
    packet::UdpLink ln;
    std::string err;
    REQUIRE(ln.start(59343, err, "127.0.0.1", 59342));
    packet::UdpLink dev;
    CHECK(dev.start(59342, err)); // 设备固定端口 59342
    dev.setRemote("127.0.0.1", 59343);
    packet::UdpLink stranger;
    CHECK(stranger.start(0, err)); // 任意临时端口
    stranger.setRemote("127.0.0.1", 59343);
    CHECK(dev.send(hex("01 00 02 00 64"), err));     // 命中
    CHECK(stranger.send(hex("01 00 02 00 FF"), err)); // 未命中：丢弃
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::deque<packet::UdpPacket> in;
    ln.drain(in);
    CHECK(in.size() == 1); // 只收到命中包
    if (!in.empty())
        CHECK(packet::bytesToHex(in.front().data) == "01 00 02 00 64");
    ln.stop();
    dev.stop();
    stranger.stop();

    // 数据源接线：dip="*"（正向全收），绑定即 dport。
    // 不再手工置 udp——监听(UDP)应自行走 UDP 收包路径
    p.pages[0].components[0].setProp("listenIp", std::string("*"));
    FrameSourceSettings st2 = frameSettingsFromProject(p);
    st2.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "值";
    f.tagId = 1;
    f.type = packet::FieldType::U16;
    f.address = 0;
    st2.fields.push_back(f);
    FrameDataSource src(st2);
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59342 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink any;
    CHECK(any.start(0, err));
    any.setRemote("127.0.0.1", 59342); // 发往 dport（= 绑定端口）
    CHECK(any.send(hex("01 00 02 00 64"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    Tag t = makeTag("值", 0, TagDataType::UInt16, 1.0);
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    CHECK(results[0].ok);
    int64_t v = 0;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) v = *i;
    if (auto* d = std::get_if<double>(&results[0].value)) v = (int64_t)*d;
    CHECK(v == 100);
    any.stop();
    src.disconnect();
}
TEST_CASE("数据目的：工程级合成（source 按名称关联数据源）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.name = "串口采集";
    ds.setProp("autoStart", true);
    ds.setProp("transport", std::string("串口"));
    ds.setProp("serialPort", std::string("COM5"));
    p.pages[0].components.push_back(ds);

    Component sk = ComponentRegistry::createComponent("DataSink", "sink-1");
    sk.name = "上传";
    sk.setProp("source", std::string("串口采集"));
    sk.setProp("transport", std::string("TCP"));
    sk.setProp("tcpRole", std::string("客户端"));
    sk.setProp("host", std::string("192.168.1.9"));
    sk.setProp("remotePort", int64_t(7100));
    p.pages[0].components.push_back(sk);

    // 未关联源的 sink 也要进入列表（运行器按 sourceName 过滤，校验面板提示）
    Component sk2 = ComponentRegistry::createComponent("DataSink", "sink-2");
    p.pages[0].components.push_back(sk2);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    CHECK(s.serial);                 // 数据源本体
    CHECK(s.sourceName == "串口采集"); // 生效数据源名（供 sink 关联匹配）
    REQUIRE(s.sinks.size() == 2);
    CHECK(s.sinks[0].sourceName == "串口采集");
    CHECK(!s.sinks[0].udp && !s.sinks[0].serial);           // TCP
    CHECK(s.sinks[0].tcpClient);                             // 客户端角色
    CHECK(s.sinks[0].host == "192.168.1.9");
    CHECK(s.sinks[0].remotePort == 7100);
    CHECK(s.sinks[1].sourceName.empty()); // 未关联

    // 默认数据目的（未改属性）：TCP + 客户端角色（注册表默认传输 TCP）
    Component fresh = ComponentRegistry::createComponent("DataSink", "sink-3");
    fresh.setProp("source", std::string("串口采集"));
    p.pages[0].components.clear();
    p.pages[0].components.push_back(ds);
    p.pages[0].components.push_back(fresh);
    FrameSourceSettings d = frameSettingsFromProject(p);
    REQUIRE(d.sinks.size() == 1);
    CHECK(!d.sinks[0].udp && !d.sinks[0].serial); // TCP
    CHECK(d.sinks[0].tcpClient);
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
    ds.setProp("autoStart", true);
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
    fresh.setProp("autoStart", true);
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

TEST_CASE("帧数据源：autoStart 默认关（打开不主动连，可手动启动）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    // 默认（未设置 autoStart）：配置仍生效（enabled=true），仅不自动启动
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("transport", std::string("UDP"));
    ds.setProp("protocol", std::string("某协议"));
    p.pages[0].components.push_back(ds);
    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);        // 帧数据源已配置
    CHECK(!s.autoStart);     // 默认关：PageViewer 打开后手动启动

    // 勾选自动启动 -> autoStart=true
    p.pages[0].components[0].setProp("autoStart", true);
    FrameSourceSettings s2 = frameSettingsFromProject(p);
    CHECK(s2.enabled);
    CHECK(s2.autoStart);

    // 多个数据源：仍取首个（autoStart 不影响选源，仅控制自动连接）
    Component second = ComponentRegistry::createComponent("DataSource", "ds-2");
    second.setProp("transport", std::string("串口"));
    second.setProp("serialPort", std::string("COM5"));
    p.pages[0].components.push_back(second);
    FrameSourceSettings s3 = frameSettingsFromProject(p);
    CHECK(s3.udp); // 生效者仍是排前的 ds-1
}

TEST_CASE("帧数据源：串口传输的工程级合成") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
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
    fresh.setProp("autoStart", true);
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
        ds.setProp("autoStart", true);
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
        ds.setProp("autoStart", true);
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

TEST_CASE("帧日志裁剪不影响数据目的转发队列") {
    // 回归：转发曾搭在 200 条上限的监视日志上，单次 pump 超过 200 帧即丢转发帧。
    // 解耦后转发队列必须拿到全部帧，监视日志仍保持最近 200 条
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.udp = true;
    cfg.localPort = 59362;
    cfg.remotePort = 59362;
    cfg.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "温度";
    f.tagId = 1;
    f.offset = 0;
    f.type = packet::FieldType::U16;
    f.address = 0;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    std::string err;
    if (!src.connect(err)) {
        std::printf("    [skip] 端口 59362 绑定失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    CHECK(sender.start(0, err));
    sender.setRemote("127.0.0.1", 59362);

    // 分 5 批 × 100 帧发送（批间间歇防内核收包缓冲溢出），期间不 pump：
    // 500 帧全部到齐后单次 readTags 触发一次 pump
    for (int b = 0; b < 5; ++b) {
        for (int i = 0; i < 100; ++i)
            CHECK(sender.send(hex("01 00 02 01 F4"), err));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Tag t = makeTag("温度", 0, TagDataType::UInt16, 1.0);
    std::vector<const Tag*> tags = {&t};
    (void)src.readTags(tags); // 单次 pump 全部 500 帧

    std::deque<std::vector<uint8_t>> fwd;
    src.drainForwardFrames(fwd);
    CHECK(fwd.size() == 500);  // 转发队列：一帧不少
    if (!fwd.empty())
        CHECK(packet::bytesToHex(fwd.front()) == "01 00 02 01 F4");
    std::deque<FrameDataSource::FrameLogEntry> log;
    src.drainFrameLog(log);
    CHECK(log.size() == 200); // 监视日志：仍是最多 200 条
    CHECK(src.matchedFrameCount() == 500);

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
