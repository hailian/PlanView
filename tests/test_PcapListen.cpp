// 镜像抓包（Npcap/SPAN）单测：BPF 表达式 / 以太帧解析 / 逐流拆帧 / 配置合成 /
// 失败路径。回环端到端仅在装有 Npcap 环回设备时执行，否则按惯例 [skip]。
#include "PvTest.h"

#include <chrono>
#include <thread>

#include "base/data/frame/FrameDataSource.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/HexUtil.h"
#include "base/packet/PcapLink.h"
#include "base/packet/UdpLink.h"

using namespace pv;

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    std::string err;
    if (!packet::hexToBytes(s, v, err))
        std::printf("    [hex] 解析失败: %s\n", err.c_str());
    return v;
}

static Tag makeTag(const char* name, int address, TagDataType type) {
    Tag t;
    t.name = name;
    t.address = address;
    t.type = type;
    return t;
}

static void push16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

// 构造 ETH(+VLAN)+IPv4+TCP/UDP 测试帧（校验和一律 0，解析器不校验）
static std::vector<uint8_t> buildTestFrame(bool withVlan, bool tcp, const uint8_t srcIp[4],
                                           const uint8_t dstIp[4], uint16_t sport,
                                           uint16_t dport, const std::vector<uint8_t>& payload,
                                           size_t truncateTo = 0) {
    std::vector<uint8_t> f;
    f.insert(f.end(), 6, 0x02); // dst MAC
    f.insert(f.end(), 6, 0x03); // src MAC
    if (withVlan) {
        push16(f, 0x8100);
        push16(f, 0x0064); // TCI：VID 100
    }
    push16(f, 0x0800);
    size_t l4Len = (tcp ? 20 : 8) + payload.size();
    f.push_back(0x45);                 // version=4, IHL=5
    f.push_back(0);                    // TOS
    push16(f, (uint16_t)(20 + l4Len)); // 总长
    push16(f, 0);                      // ID
    push16(f, 0);                      // flags/frag
    f.push_back(64);                   // TTL
    f.push_back(tcp ? 6 : 17);         // 协议
    push16(f, 0);                      // 头校验和（不校验）
    f.insert(f.end(), srcIp, srcIp + 4);
    f.insert(f.end(), dstIp, dstIp + 4);
    push16(f, sport);
    push16(f, dport);
    if (tcp) {
        push16(f, 0);
        push16(f, 0);       // seq
        push16(f, 0);
        push16(f, 0);       // ack
        f.push_back(0x50);  // data offset = 5
        f.push_back(0x18);  // PSH|ACK
        push16(f, 0x2000);  // window
        push16(f, 0);       // checksum
        push16(f, 0);       // urgent
    } else {
        push16(f, (uint16_t)(8 + payload.size())); // UDP 长度
        push16(f, 0);                              // checksum
    }
    f.insert(f.end(), payload.begin(), payload.end());
    if (truncateTo && truncateTo < f.size()) f.resize(truncateTo);
    return f;
}

TEST_CASE("监听镜像：BPF 三元组表达式（含 VLAN 放行）") {
    // 固定 dip：host + proto port（命中 = 任一方向，镜像场景收完整会话）
    CHECK(packet::buildListenBpf("192.168.1.50", 9002, false) ==
          "(host 192.168.1.50 and udp port 9002) or (vlan and host 192.168.1.50 and udp "
          "port 9002)");
    CHECK(packet::buildListenBpf("10.1.1.9", 502, true) ==
          "(host 10.1.1.9 and tcp port 502) or (vlan and host 10.1.1.9 and tcp port 502)");
    // dip 通配（或空）：只按端口，任一方向
    CHECK(packet::buildListenBpf("*", 9002, true) ==
          "(tcp port 9002) or (vlan and tcp port 9002)");
    CHECK(packet::buildListenBpf("", 9002, true) == packet::buildListenBpf("*", 9002, true));
}

TEST_CASE("监听镜像：以太帧解析（UDP/TCP/VLAN/非法帧）") {
    const uint8_t src[4] = {10, 0, 0, 1}, dst[4] = {10, 0, 0, 2};
    packet::CapPacket p;

    // 普通 UDP 帧：四元组 + 负载 + 流标识
    auto udp = buildTestFrame(false, false, src, dst, 4001, 9002, hex("01 00 02 00 64"));
    REQUIRE(packet::parseEthernetFrame(udp.data(), udp.size(), p));
    CHECK(p.srcIp == "10.0.0.1");
    CHECK(p.dstIp == "10.0.0.2");
    CHECK(p.srcPort == 4001);
    CHECK(p.dstPort == 9002);
    CHECK(!p.tcp);
    CHECK(p.payload == hex("01 00 02 00 64"));
    CHECK(p.flowKey == "10.0.0.1:4001>10.0.0.2:9002");

    // VLAN 单标签（SPAN 口常见）：剥标签后同样解出
    auto vlan = buildTestFrame(true, false, src, dst, 4001, 9002, hex("AA"));
    REQUIRE(packet::parseEthernetFrame(vlan.data(), vlan.size(), p));
    CHECK(p.srcIp == "10.0.0.1");
    CHECK(p.dstPort == 9002);
    CHECK(p.payload == hex("AA"));

    // TCP：data offset=5，负载紧跟 20B 头
    auto tcp = buildTestFrame(false, true, dst, src, 9002, 4001, hex("11 22 33"));
    REQUIRE(packet::parseEthernetFrame(tcp.data(), tcp.size(), p));
    CHECK(p.tcp);
    CHECK(p.srcPort == 9002);
    CHECK(p.dstPort == 4001);
    CHECK(p.payload == hex("11 22 33"));
    CHECK(p.flowKey == "10.0.0.2:9002>10.0.0.1:4001");

    // 非法/无关帧一律拒绝：过短、IPv6、坏 IHL、抓包截断、非 TCP/UDP
    CHECK(!packet::parseEthernetFrame(udp.data(), 13, p));
    auto v6 = buildTestFrame(false, false, src, dst, 1, 2, hex("00"));
    v6[12] = 0x86;
    v6[13] = 0xDD;
    CHECK(!packet::parseEthernetFrame(v6.data(), v6.size(), p));
    auto badIhl = buildTestFrame(false, false, src, dst, 1, 2, hex("00 00"));
    badIhl[14] = 0x4F; // IHL=15 → 60B > IP 总长
    CHECK(!packet::parseEthernetFrame(badIhl.data(), badIhl.size(), p));
    auto cut = buildTestFrame(false, false, src, dst, 1, 2, hex("00 00"));
    cut.resize(cut.size() - 2); // 尾部截断：IP 总长超出实际抓到的字节
    CHECK(!packet::parseEthernetFrame(cut.data(), cut.size(), p));
    auto icmp = buildTestFrame(false, false, src, dst, 1, 2, hex("00"));
    icmp[23] = 1; // 协议 = ICMP
    CHECK(!packet::parseEthernetFrame(icmp.data(), icmp.size(), p));
}

TEST_CASE("监听镜像：TCP 逐流拆帧与 UDP 直通") {
    packet::FramingConfig cfg;
    cfg.mode = packet::FrameMode::Tlv; // T=1B L=2B 大端
    packet::FlowSplitters flows(cfg);

    // 两路 TCP 流（A→B / B→A）交错喂：帧跨包到达，互不掺杂
    packet::CapPacket ab, ba, udp;
    ab.srcIp = "10.0.0.1";
    ab.dstIp = "10.0.0.2";
    ab.srcPort = 4001;
    ab.dstPort = 9002;
    ab.tcp = true;
    ab.flowKey = "10.0.0.1:4001>10.0.0.2:9002";
    ba = ab;
    ba.srcIp = "10.0.0.2";
    ba.dstIp = "10.0.0.1";
    ba.srcPort = 9002;
    ba.dstPort = 4001;
    ba.flowKey = "10.0.0.2:9002>10.0.0.1:4001";

    std::vector<std::vector<uint8_t>> out;
    ab.payload = hex("01 00 04 11 22"); // 流1 前半帧（T=01 L=04）
    flows.feed(ab, out);
    CHECK(out.empty()); // 未成帧
    ba.payload = hex("02 00 03 AA BB CC"); // 流2 完整一帧
    flows.feed(ba, out);
    REQUIRE(out.size() == 1); // 流2 先成帧
    CHECK(out[0] == hex("02 00 03 AA BB CC"));
    ab.payload = hex("33 44"); // 流1 后半帧
    flows.feed(ab, out);
    REQUIRE(out.size() == 2);
    CHECK(out[1] == hex("01 00 04 11 22 33 44"));

    // UDP 数据报直通：一包一帧（不做拆帧）
    out.clear();
    udp.payload = hex("01 00 02 00 64");
    flows.feed(udp, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == hex("01 00 02 00 64"));

    // reset 清空各流缓冲（连接重建）
    flows.reset();
    out.clear();
    ab.payload = hex("33 44"); // 残余半帧已清，不吐帧
    flows.feed(ab, out);
    CHECK(out.empty());
}

TEST_CASE("监听镜像：工程配置合成（listenMode/listenNic）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("transport", std::string("监听"));
    ds.setProp("listenMode", std::string("镜像抓包"));
    ds.setProp("listenNic", std::string("\\Device\\NPF\\{ABCD-1234}"));
    ds.setProp("listenIp", std::string("192.168.1.50"));
    ds.setProp("listenPort", int64_t(502));
    ds.setProp("listenProto", std::string("TCP"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings st = frameSettingsFromProject(p);
    CHECK(st.enabled);
    CHECK(st.listen);
    CHECK(st.listenPcap);
    CHECK(st.listenNic == "\\Device\\NPF\\{ABCD-1234}");
    CHECK(st.listenTcp);
    CHECK(st.listenIp == "192.168.1.50");
    CHECK(st.listenPort == 502);

    // 旧工程无 listenMode 属性：默认「本机端口」，行为不变
    p.pages[0].components[0].props.erase("listenMode");
    st = frameSettingsFromProject(p);
    CHECK(st.listen);
    CHECK(!st.listenPcap);
}

TEST_CASE("监听镜像：未选网卡/设备不存在时启动失败") {
    packet::PcapLink ln;
    std::string err;
    CHECK(!ln.start("", "udp port 9002", err)); // 未选网卡（无须 Npcap 即可判定）
    CHECK(err.find("网卡") != std::string::npos);

    // 数据源接线同路：连接失败报错，不崩溃
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.listen = true;
    cfg.listenPcap = true;
    cfg.listenPort = 9002;
    cfg.framing.mode = packet::FrameMode::Tlv;
    FrameDataSource src(cfg);
    CHECK(!src.connect(err));
    CHECK(err.find("网卡") != std::string::npos);

    // 幽灵设备名：未装 Npcap 报安装提示，装了报打开失败——两者都必须是失败
    CHECK(!ln.start("\\Device\\NPF\\{00000000-0000-0000-0000-000000000000}",
                    "udp port 9002", err));
}

TEST_CASE("监听镜像：回环抓包端到端（需 Npcap 环回设备，否则跳过）") {
    std::string err;
    auto adapters = packet::npcap::listAdapters(err);
    if (adapters.empty()) {
        std::printf("    [skip] %s\n", err.c_str());
        return;
    }
    std::string loopDev;
    for (const auto& a : adapters)
        if (a.first.find("Loopback") != std::string::npos ||
            a.second.find("loopback") != std::string::npos)
            loopDev = a.first;
    if (loopDev.empty()) {
        std::printf("    [skip] 无 Npcap 环回设备（仅安装 Npcap 时勾选环回支持）\n");
        return;
    }

    const int dport = 59351;
    FrameSourceSettings cfg;
    cfg.enabled = true;
    cfg.listen = true;
    cfg.listenPcap = true;
    cfg.listenNic = loopDev;
    cfg.listenIp = "*";
    cfg.listenPort = dport;
    cfg.listenTcp = false;
    cfg.framing.mode = packet::FrameMode::Tlv;
    TagField f;
    f.name = "值";
    f.tagId = 1;
    f.type = packet::FieldType::U16;
    f.address = 0;
    cfg.fields.push_back(f);

    FrameDataSource src(cfg);
    if (!src.connect(err)) {
        std::printf("    [skip] 环回设备打开失败: %s\n", err.c_str());
        return;
    }
    packet::UdpLink sender;
    REQUIRE(sender.start(0, err)); // 临时源端口
    sender.setRemote("127.0.0.1", dport); // 发往 dport（BPF 正向命中）
    CHECK(sender.send(hex("01 00 02 00 64"), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 等抓包线程收到
    Tag t = makeTag("值", 0, TagDataType::UInt16);
    std::vector<const Tag*> tags = {&t};
    auto results = src.readTags(tags);
    int64_t v = 0;
    if (auto* i = std::get_if<int64_t>(&results[0].value)) v = *i;
    if (auto* d = std::get_if<double>(&results[0].value)) v = (int64_t)*d;
    CHECK(results[0].ok);
    CHECK(v == 100);
    sender.stop();
    src.disconnect();
}
