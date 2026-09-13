// M7 单测：报文工具 — HEX 转换 / TCP 拆帧（TLV、帧头+Length）/ 规约解析 / 配置 JSON / UDP 回环
#include "SoftgTest.h"

#include <chrono>
#include <thread>

#include "base/packet/DebugConfig.h"
#include "base/packet/FrameCodec.h"
#include "base/packet/HexUtil.h"
#include "base/packet/PacketSpec.h"
#include "base/packet/UdpLink.h"

using namespace softg::packet;

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    std::string err;
    // 注意：SoftgTest 的 CHECK/REQUIRE 宏含 return 语句，不能在非 void 函数中使用
    if (!hexToBytes(s, v, err))
        std::printf("    [hex] 解析失败: %s\n", err.c_str());
    return v;
}

TEST_CASE("HEX 转换往返") {
    auto v = hex("AA 55 01 2b");
    CHECK(v.size() == 4);
    CHECK(v[0] == 0xAA && v[3] == 0x2B);
    CHECK(bytesToHex(v) == "AA 55 01 2B");

    // 无分隔与混合分隔
    CHECK(hex("A55501")[0] == 0xA5);
    CHECK(hex("AA,55:01\n02").size() == 4);

    std::vector<uint8_t> out;
    std::string err;
    CHECK(!hexToBytes("GG", out, err));   // 非法字符
    CHECK(!hexToBytes("ABC", out, err));  // 奇数位
    CHECK(bytesToHex({}) == "");
}

TEST_CASE("TLV 拆帧：分段到达 + 失步恢复") {
    FramingConfig cfg; // 默认 T=1 L=2 大端，L 不含帧头
    FrameSplitter sp(cfg);

    // 帧1: T=01 L=0003 V=AA BB CC；帧2: T=02 L=0001 V=FF
    std::vector<uint8_t> stream = hex("01 00 03 AA BB CC 02 00 01 FF");
    std::vector<std::vector<uint8_t>> out;
    // 逐字节喂入（最恶劣的分段）
    for (uint8_t b : stream) sp.feed(&b, 1, out);
    CHECK(out.size() == 2);
    CHECK(bytesToHex(out[0]) == "01 00 03 AA BB CC");
    CHECK(bytesToHex(out[1]) == "02 00 01 FF");

    // 超长 L 越限：不产出帧（上限保护；TLV 无同步特征，靠 L 越限丢弃滑窗）
    out.clear();
    auto over = hex("01 7F 7F"); // L=32639 > 帧长上限 → 丢弃后重试
    sp.feed(over.data(), over.size(), out);
    CHECK(out.empty());
}

TEST_CASE("TLV 拆帧：L 含帧头 + 小端") {
    FramingConfig cfg;
    cfg.lenBytes = 2;
    cfg.bigEndian = false;
    cfg.lenIncludesHeader = true;
    FrameSplitter sp(cfg);

    // L=0005(LE) 含 T+L，负载 2 字节 → 帧 = 07 05 00 AA BB
    auto stream = hex("07 05 00 AA BB 07 05 00 11 22");
    std::vector<std::vector<uint8_t>> out;
    sp.feed(stream.data(), stream.size(), out);
    CHECK(out.size() == 2);
    CHECK(bytesToHex(out[0]) == "07 05 00 AA BB");
    CHECK(bytesToHex(out[1]) == "07 05 00 11 22");
}

TEST_CASE("帧头+Length 拆帧：同步 + 分段 + length 含义") {
    FramingConfig cfg;
    cfg.mode = FrameMode::HeaderLength;
    cfg.header = hex("AA 55");
    cfg.lenOffset = 2;
    cfg.lenBytesHeader = 2;
    cfg.bigEndianHeader = true;
    cfg.lenIncludesAll = false; // length 只计负载
    FrameSplitter sp(cfg);

    // 帧: AA 55 00 03 + 3 字节负载; 前面塞 2 字节垃圾 11 22 AA 触发重同步
    auto stream = hex("11 22 AA AA 55 00 03 01 02 03 AA 55 00 01 EE");
    std::vector<std::vector<uint8_t>> out;
    // 两段非对齐喂入
    sp.feed(stream.data(), 5, out);
    sp.feed(stream.data() + 5, stream.size() - 5, out);
    CHECK(out.size() == 2);
    CHECK(bytesToHex(out[0]) == "AA 55 00 03 01 02 03");
    CHECK(bytesToHex(out[1]) == "AA 55 00 01 EE");

    // length 为整帧长模式：length=0009 表示整帧 9 字节
    cfg.lenIncludesAll = true;
    sp.setConfig(cfg);
    out.clear();
    auto stream2 = hex("AA 55 00 09 99 88 77 66 55");
    sp.feed(stream2.data(), stream2.size(), out);
    CHECK(out.size() == 1);
    CHECK(bytesToHex(out[0]) == "AA 55 00 09 99 88 77 66 55");
}

TEST_CASE("规约解析：整数/浮点/字节序/工程换算") {
    PacketSpec spec;
    PacketField f;
    f.name = "温度";
    f.offset = 0;
    f.type = FieldType::U16;
    f.bigEndian = true;
    f.scale = 0.1;
    spec.fields.push_back(f);

    f.name = "电流";
    f.offset = 2;
    f.type = FieldType::I16;
    f.bigEndian = false; // 小端
    f.scale = 1.0;
    f.offsetValue = 0.0;
    spec.fields.push_back(f);

    f.name = "功率";
    f.offset = 4;
    f.type = FieldType::F32;
    f.bigEndian = true;
    spec.fields.push_back(f);

    f.name = "标签";
    f.offset = 8;
    f.type = FieldType::Ascii;
    f.length = 3;
    spec.fields.push_back(f);

    f.name = "越界字段";
    f.offset = 100;
    f.type = FieldType::U16;
    spec.fields.push_back(f);

    auto payload = hex("01 F4 00 05 42 28 00 00 4F 4B 21"); // 500*0.1=50; -? ; 42 28 00 00 = 42.0; "OK!"
    auto parsed = parsePacket(spec.fields, payload);
    CHECK(parsed.size() == 5);

    CHECK(parsed[0].ok);
    CHECK(parsed[0].rawText == "500");
    CHECK(parsed[0].engText == "50");

    CHECK(parsed[1].ok);
    CHECK(parsed[1].rawText == "1280"); // 小端 00 05 → 0x0500 = 1280
    CHECK(parsed[1].rawHex == "00 05");

    CHECK(parsed[2].ok);
    CHECK(parsed[2].rawText == "42");
    CHECK(parsed[2].engText == "42");

    CHECK(parsed[3].rawText == "OK!");
    CHECK(parsed[3].engText == "OK!");

    CHECK(!parsed[4].ok);
    CHECK(parsed[4].rawText == "(越界)");
}

TEST_CASE("规约解析：布尔/字符串/枚举/双精度") {
    PacketSpec spec;
    PacketField f;
    f.name = "开关";
    f.offset = 0;
    f.type = FieldType::Bool; // 1 字节
    spec.fields.push_back(f);

    f.name = "状态";
    f.offset = 1;
    f.type = FieldType::Enum;
    f.length = 1;
    f.bigEndian = true;
    f.enums = {{0, "停止"}, {1, "运行"}, {2, "故障"}};
    spec.fields.push_back(f);

    f.name = "名称";
    f.offset = 2;
    f.type = FieldType::String;
    f.length = 4; // 定长，尾部 0x00 填充
    spec.fields.push_back(f);

    f.name = "精度";
    f.offset = 6;
    f.type = FieldType::F64;
    f.bigEndian = true;
    spec.fields.push_back(f);

    f.name = "未知枚举";
    f.offset = 1;
    f.type = FieldType::Enum;
    f.length = 1;
    f.enums = {{0, "停止"}}; // 值为 1 未命中 → 回退数值文本
    spec.fields.push_back(f);

    // 01 | 01 | 'O''K''!'00 | f64(1.5)=3F F8 00 00 00 00 00 00
    auto payload = hex("01 01 4F 4B 21 00 3F F8 00 00 00 00 00 00");
    auto parsed = parsePacket(spec.fields, payload);
    REQUIRE(parsed.size() == 5);
    CHECK(parsed[0].rawText == "true");
    CHECK(parsed[1].rawText == "运行"); // 枚举命中 → 名称
    CHECK(parsed[2].rawText == "OK!");  // 去尾部 NUL
    CHECK(parsed[3].rawText == "1.5");  // f64
    CHECK(parsed[4].rawText == "1");    // 未命中 → 数值
}

TEST_CASE("接入配置 JSON 保存/加载往返") {
    DebugConfig cfg;
    cfg.transport = Transport::Tcp;
    cfg.host = "192.168.1.10";
    cfg.remotePort = 8888;
    cfg.localPort = 7777;
    cfg.framing.mode = FrameMode::HeaderLength;
    cfg.framing.header = hex("AA 55");
    cfg.framing.lenOffset = 2;
    cfg.framing.lenBytesHeader = 2;
    cfg.framing.lenIncludesAll = true;
    cfg.spec.name = "设备规约";
    PacketField f;
    f.name = "液位";
    f.offset = 4;
    f.type = FieldType::F32;
    f.scale = 0.01;
    f.offsetValue = -5.0;
    cfg.spec.fields.push_back(f);

    PacketField fe;
    fe.name = "模式";
    fe.offset = 8;
    fe.type = FieldType::Enum;
    fe.length = 1;
    fe.enums = {{0, "手动"}, {1, "自动"}};
    cfg.spec.fields.push_back(fe);

    std::string err;
    const char* path = "out/test_debug_config.json";
    CHECK(debugcfg::save(path, cfg, err));

    DebugConfig loaded;
    CHECK(debugcfg::load(path, loaded, err));
    CHECK(loaded.transport == Transport::Tcp);
    CHECK(loaded.host == "192.168.1.10");
    CHECK(loaded.remotePort == 8888);
    CHECK(loaded.localPort == 7777);
    CHECK(loaded.framing.mode == FrameMode::HeaderLength);
    CHECK(bytesToHex(loaded.framing.header) == "AA 55");
    CHECK(loaded.framing.lenOffset == 2);
    CHECK(loaded.framing.lenIncludesAll == true);
    CHECK(loaded.spec.name == "设备规约");
    REQUIRE(loaded.spec.fields.size() == 2);
    CHECK(loaded.spec.fields[0].type == FieldType::F32);
    CHECK(loaded.spec.fields[0].offset == 4);
    CHECK(loaded.spec.fields[0].scale == 0.01);
    CHECK(loaded.spec.fields[0].offsetValue == -5.0);
    // 枚举类型 + 映射表往返
    CHECK(loaded.spec.fields[1].type == FieldType::Enum);
    CHECK(loaded.spec.fields[1].length == 1);
    REQUIRE(loaded.spec.fields[1].enums.size() == 2);
    CHECK(loaded.spec.fields[1].enums[0].second == "手动");
    CHECK(loaded.spec.fields[1].enums[1].first == 1);
    CHECK(loaded.spec.fields[1].enums[1].second == "自动");
}

TEST_CASE("UDP 回环：发送 + 接收 + 规约解析") {
    UdpLink link;
    std::string err;
    // 绑定一个已知端口，被占用则跳过该用例
    int port = 59123;
    if (!link.start(port, err)) {
        std::printf("    [skip] 端口 %d 被占用，跳过 UDP 回环\n", port);
        return;
    }
    link.setRemote("127.0.0.1", port);

    auto payload = hex("01 F4 42 28 00 00");
    CHECK(link.send(payload, err));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::deque<UdpPacket> in;
    link.drain(in);
    CHECK(in.size() == 1);
    if (in.size() == 1) {
        CHECK(bytesToHex(in.front().data) == "01 F4 42 28 00 00");
        CHECK(in.front().from.find("127.0.0.1") == 0);

        PacketField f;
        f.name = "温度";
        f.offset = 0;
        f.type = FieldType::U16;
        f.scale = 0.1;
        PacketField f2;
        f2.name = "功率";
        f2.offset = 2;
        f2.type = FieldType::F32;
        auto parsed = parsePacket({f, f2}, in.front().data);
        CHECK(parsed.size() == 2);
        CHECK(parsed[0].engText == "50");
        CHECK(parsed[1].rawText == "42");
    }
    link.stop();
}
