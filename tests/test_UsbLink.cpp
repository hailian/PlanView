// USB 传输单测：设备 token/端点解析（纯逻辑，无 DLL 也全跑）、注册表枚举、
// 工程设置合成（数据源/数据目的 USB 分支，含 tcpClient 不误落 TCP 的回归）、
// FrameDataSource 配置校验（先于驱动检查，与镜像抓包幽灵网卡用例同策略）。
// 真机枚举用例：未放置 libusb-1.0.dll / 无可打开设备时自动 [skip]。
#include "PvTest.h"

#include <cstdio>

#include "base/data/frame/FrameDataSource.h"
#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/UsbApi.h"
#include "base/packet/UsbLink.h"

using namespace pv;
using pv::packet::makeDeviceToken;
using pv::packet::parseDeviceToken;
using pv::packet::parseEndpointHex;

// ---- 设备 token："vid:pid[:serial]" ----

TEST_CASE("USB 设备 token 解析：正反例") {
    uint16_t vid = 0, pid = 0;
    std::string serial, err;

    REQUIRE(parseDeviceToken("0483:5740", vid, pid, serial, err));
    CHECK(vid == 0x0483);
    CHECK(pid == 0x5740);
    CHECK(serial.empty());

    REQUIRE(parseDeviceToken("483:5740", vid, pid, serial, err)); // 1-4 位十六进制均可
    CHECK(vid == 0x483);
    CHECK(pid == 0x5740);

    REQUIRE(parseDeviceToken("0483:5740:SN123", vid, pid, serial, err)); // 带序列号
    CHECK(vid == 0x0483);
    CHECK(pid == 0x5740);
    CHECK(serial == "SN123");

    REQUIRE(parseDeviceToken("abcd:EF", vid, pid, serial, err)); // 大小写不敏感
    CHECK(vid == 0xABCD);
    CHECK(pid == 0xEF);

    // 反例：空 / 无冒号 / 非十六进制 / 段超 4 位 / 尾随空序列号
    CHECK(!parseDeviceToken("", vid, pid, serial, err));
    CHECK(!parseDeviceToken("04835740", vid, pid, serial, err));
    CHECK(!parseDeviceToken("0483", vid, pid, serial, err));
    CHECK(!parseDeviceToken("xyz:5740", vid, pid, serial, err));
    CHECK(!parseDeviceToken("12345:5740", vid, pid, serial, err)); // 超 16 位
    CHECK(!parseDeviceToken("0483:5740:", vid, pid, serial, err)); // 序列号段为空
}

TEST_CASE("USB 设备 token 生成与解析往返") {
    CHECK(makeDeviceToken(0x0483, 0x5740, "") == "0483:5740");
    CHECK(makeDeviceToken(0x0483, 0x5740, "SN9") == "0483:5740:SN9");
    uint16_t vid = 0, pid = 0;
    std::string serial, err;
    REQUIRE(parseDeviceToken(makeDeviceToken(0xABCD, 0x12, "X1"), vid, pid, serial, err));
    CHECK(vid == 0xABCD);
    CHECK(pid == 0x12);
    CHECK(serial == "X1");
}

// ---- 端点 hex 文本 ----

TEST_CASE("USB 端点 hex 解析：正反例") {
    uint8_t ep = 0;
    std::string err;
    REQUIRE(parseEndpointHex("81", ep, err));
    CHECK(ep == 0x81);
    REQUIRE(parseEndpointHex("01", ep, err));
    CHECK(ep == 0x01);
    REQUIRE(parseEndpointHex("0xE1", ep, err)); // 容忍 0x 前缀
    CHECK(ep == 0xE1);
    REQUIRE(parseEndpointHex("Ab", ep, err)); // 大小写不敏感
    CHECK(ep == 0xAB);

    CHECK(!parseEndpointHex("", ep, err));    // 空 = 自动选择，非合法端点值
    CHECK(parseEndpointHex("8", ep, err));    // 1 位也合法
    CHECK(ep == 0x08);
    CHECK(!parseEndpointHex("813", ep, err)); // 超 2 位
    CHECK(!parseEndpointHex("zz", ep, err));  // 非十六进制
    CHECK(!parseEndpointHex("0x", ep, err));
}

// ---- 注册表：USB 传输枚举与属性 ----

TEST_CASE("通信组件注册表含 USB 传输与属性") {
    auto hasKey = [](const ComponentTypeInfo* info, const char* key) {
        for (const auto& s : info->properties)
            if (s.key == key) return true;
        return false;
    };
    auto enumHas = [](const ComponentTypeInfo* info, const char* key, const char* val) {
        for (const auto& s : info->properties)
            if (s.key == key)
                for (const auto& v : s.enumValues)
                    if (v == val) return true;
        return false;
    };
    auto& reg = ComponentRegistry::instance();
    const ComponentTypeInfo* ds = reg.find("DataSource");
    REQUIRE(ds != nullptr);
    CHECK(enumHas(ds, "transport", "USB"));
    // 数据源 USB 属性四件套（设备/接口/IN/OUT 端点）
    CHECK(hasKey(ds, "usbDevice"));
    CHECK(hasKey(ds, "usbInterface"));
    CHECK(hasKey(ds, "usbEpIn"));
    CHECK(hasKey(ds, "usbEpOut"));

    const ComponentTypeInfo* sk = reg.find("DataSink");
    REQUIRE(sk != nullptr);
    CHECK(enumHas(sk, "transport", "USB"));
    // 数据目的转发只发不收：设备/接口/OUT 端点（无 IN）
    CHECK(hasKey(sk, "usbDevice"));
    CHECK(hasKey(sk, "usbInterface"));
    CHECK(hasKey(sk, "usbEpOut"));
    CHECK(!hasKey(sk, "usbEpIn"));
}

// ---- 工程设置合成 ----

TEST_CASE("工程设置合成：数据源传输=USB") {
    Project p;
    p.pages.push_back(Page{}); // Project 默认无页面，与现有测试同法显式建页
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
    ds.setProp("transport", std::string("USB"));
    ds.setProp("usbDevice", std::string("0483:5740:SN1"));
    ds.setProp("usbInterface", int64_t(2));
    ds.setProp("usbEpIn", std::string("82"));
    ds.setProp("usbEpOut", std::string("02"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    CHECK(s.usb);
    CHECK(!s.udp);
    CHECK(!s.serial);
    CHECK(!s.listen);
    CHECK(!s.autoSend);
    CHECK(!s.tcpClient); // USB 不得误落 TCP 分支（回归点）
    CHECK(s.usbDevice == "0483:5740:SN1");
    CHECK(s.usbInterface == 2);
    CHECK(s.usbEpIn == "82");
    CHECK(s.usbEpOut == "02");

    // 旧工程缺省：数据源组件存在但 transport=UDP → USB 字段保持默认
    p.pages[0].components.back().setProp("transport", std::string("UDP"));
    FrameSourceSettings old = frameSettingsFromProject(p);
    CHECK(old.enabled);
    CHECK(!old.usb);
    CHECK(old.udp);
    CHECK(old.usbDevice.empty());
    CHECK(old.usbInterface == 0);
}

TEST_CASE("工程设置合成：数据目的 USB 转发") {
    Project p;
    p.pages.push_back(Page{}); // Project 默认无页面，与现有测试同法显式建页
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.name = "源1";
    ds.setProp("transport", std::string("USB"));
    ds.setProp("usbDevice", std::string("0483:5740"));
    p.pages[0].components.push_back(ds);

    Component sk = ComponentRegistry::createComponent("DataSink", "sk-1");
    sk.setProp("source", std::string("源1"));
    sk.setProp("transport", std::string("USB"));
    sk.setProp("usbDevice", std::string("1209:8888"));
    sk.setProp("usbInterface", int64_t(1));
    sk.setProp("usbEpOut", std::string("03"));
    p.pages[0].components.push_back(sk);

    FrameSourceSettings s = frameSettingsFromProject(p);
    REQUIRE(s.sinks.size() == 1);
    CHECK(s.sinks[0].usb);
    CHECK(!s.sinks[0].udp);
    CHECK(!s.sinks[0].serial);
    CHECK(s.sinks[0].usbDevice == "1209:8888");
    CHECK(s.sinks[0].usbInterface == 1);
    CHECK(s.sinks[0].usbEpOut == "03");

    // 非 USB 数据目的不受影响（默认 TCP）
    p.pages[0].components.back().setProp("transport", std::string("TCP"));
    FrameSourceSettings s2 = frameSettingsFromProject(p);
    CHECK(!s2.sinks[0].usb);
    CHECK(s2.sinks[0].tcpClient);
}

// ---- 帧数据源连接配置校验（先于驱动检查：无 libusb-1.0.dll 的机器同样可跑）----

TEST_CASE("帧数据源 USB 连接：坏配置确定性报错") {
    {
        FrameSourceSettings s;
        s.usb = true;
        s.usbDevice = ""; // 未选设备
        FrameDataSource src(s);
        std::string err;
        CHECK(!src.connect(err));
        CHECK(!err.empty());
        CHECK(!src.isConnected());
    }
    {
        FrameSourceSettings s;
        s.usb = true;
        s.usbDevice = "04835740"; // 缺冒号
        FrameDataSource src(s);
        std::string err;
        CHECK(!src.connect(err));
        CHECK(!err.empty());
    }
    {
        FrameSourceSettings s;
        s.usb = true;
        s.usbDevice = "0483:5740";
        s.usbEpIn = "zz"; // 端点格式非法（同样先于驱动检查）
        FrameDataSource src(s);
        std::string err;
        CHECK(!src.connect(err));
        CHECK(!err.empty());
    }
}

// ---- 真机枚举（需 libusb-1.0.dll；无 DLL/无设备自动跳过）----

TEST_CASE("USB 设备枚举（需 libusb-1.0.dll 与可打开设备，否则跳过）") {
    std::string err;
    const packet::libusb::Api* api = packet::libusb::instance(err);
    if (!api) {
        std::printf("    [skip] %s\n", err.c_str());
        return;
    }
    auto devices = packet::libusb::listDevices(err);
    if (devices.empty()) {
        std::printf("    [skip] 无可打开的 USB 设备（需 WinUSB/libusb 驱动设备接入）\n");
        return;
    }
    // 枚举到的设备 token 必须可往返解析（下拉存的 token 运行器要能匹配）
    for (const auto& d : devices) {
        std::string tok = packet::makeDeviceToken(d.vid, d.pid, d.serial);
        uint16_t vid = 0, pid = 0;
        std::string serial, perr;
        REQUIRE(packet::parseDeviceToken(tok, vid, pid, serial, perr));
        CHECK(vid == d.vid);
        CHECK(pid == d.pid);
        CHECK(serial == d.serial);
    }
}
