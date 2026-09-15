// VISA 传输单测：地址校验（纯逻辑，无 VISA 运行时也全跑）、注册表枚举、
// 工程设置合成（数据源/数据目的 VISA 分支，含 tcpClient 不误落 TCP 的回归）、
// FrameDataSource 配置校验（先于运行时检查，与镜像抓包幽灵网卡用例同策略）。
// 真机枚举用例：未装 NI-VISA/Keysight 等运行时 / 无仪器时自动 [skip]。
#include "PvTest.h"

#include <cstdio>
#include <string>

#include "base/data/frame/FrameDataSource.h"
#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/VisaApi.h"
#include "base/packet/VisaLink.h"

using namespace pv;
using pv::packet::validateVisaAddress;

// ---- VISA 资源地址校验 ----

TEST_CASE("VISA 地址校验：正反例") {
    std::string err;
    CHECK(validateVisaAddress("USB0::0x0957::0x1234::MY123456::INSTR", err));
    CHECK(validateVisaAddress("TCPIP0::192.168.1.5::inst0::INSTR", err));
    CHECK(validateVisaAddress("GPIB0::5::INSTR", err));
    CHECK(err.empty()); // 合法地址不置 err

    CHECK(!validateVisaAddress("", err)); // 空
    CHECK(!err.empty());
    err.clear();
    CHECK(!validateVisaAddress(std::string(300, 'A'), err)); // 超 VI_FIND_BUFLEN
    CHECK(!err.empty());
}

// ---- 注册表：VISA 传输枚举与属性 ----

TEST_CASE("通信组件注册表含 VISA 传输与地址属性") {
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
    CHECK(enumHas(ds, "transport", "VISA"));
    CHECK(hasKey(ds, "visaAddress"));

    const ComponentTypeInfo* sk = reg.find("DataSink");
    REQUIRE(sk != nullptr);
    CHECK(enumHas(sk, "transport", "VISA"));
    CHECK(hasKey(sk, "visaAddress"));
}

// ---- 工程设置合成 ----

TEST_CASE("工程设置合成：数据源传输=VISA") {
    Project p;
    p.pages.push_back(Page{}); // Project 默认无页面，与现有测试同法显式建页
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.setProp("autoStart", true);
    ds.setProp("transport", std::string("VISA"));
    ds.setProp("visaAddress", std::string("TCPIP0::192.168.1.5::inst0::INSTR"));
    p.pages[0].components.push_back(ds);

    FrameSourceSettings s = frameSettingsFromProject(p);
    CHECK(s.enabled);
    CHECK(s.visa);
    CHECK(!s.udp);
    CHECK(!s.serial);
    CHECK(!s.usb);
    CHECK(!s.listen);
    CHECK(!s.autoSend);
    CHECK(!s.tcpClient); // VISA 不得误落 TCP 分支（回归点）
    CHECK(s.visaAddress == "TCPIP0::192.168.1.5::inst0::INSTR");

    // 旧工程缺省：数据源组件存在但 transport=UDP → VISA 字段保持默认
    p.pages[0].components.back().setProp("transport", std::string("UDP"));
    FrameSourceSettings old = frameSettingsFromProject(p);
    CHECK(old.enabled);
    CHECK(!old.visa);
    CHECK(old.udp);
    CHECK(old.visaAddress.empty());
}

TEST_CASE("工程设置合成：数据目的 VISA 转发") {
    Project p;
    p.pages.push_back(Page{}); // Project 默认无页面，与现有测试同法显式建页
    Component ds = ComponentRegistry::createComponent("DataSource", "ds-1");
    ds.name = "源1";
    ds.setProp("transport", std::string("VISA"));
    ds.setProp("visaAddress", std::string("GPIB0::5::INSTR"));
    p.pages[0].components.push_back(ds);

    Component sk = ComponentRegistry::createComponent("DataSink", "sk-1");
    sk.setProp("source", std::string("源1"));
    sk.setProp("transport", std::string("VISA"));
    sk.setProp("visaAddress", std::string("USB0::0x0957::0x1234::MY1::INSTR"));
    p.pages[0].components.push_back(sk);

    FrameSourceSettings s = frameSettingsFromProject(p);
    REQUIRE(s.sinks.size() == 1);
    CHECK(s.sinks[0].visa);
    CHECK(!s.sinks[0].udp);
    CHECK(!s.sinks[0].serial);
    CHECK(!s.sinks[0].usb);
    CHECK(s.sinks[0].visaAddress == "USB0::0x0957::0x1234::MY1::INSTR");

    // 非 VISA 数据目的不受影响（默认 TCP）
    p.pages[0].components.back().setProp("transport", std::string("TCP"));
    FrameSourceSettings s2 = frameSettingsFromProject(p);
    CHECK(!s2.sinks[0].visa);
    CHECK(s2.sinks[0].tcpClient);
}

// ---- 帧数据源连接配置校验（先于运行时检查：无 VISA 运行时的机器同样可跑）----

TEST_CASE("帧数据源 VISA 连接：坏配置确定性报错") {
    {
        FrameSourceSettings s;
        s.visa = true;
        s.visaAddress = ""; // 未填地址
        FrameDataSource src(s);
        std::string err;
        CHECK(!src.connect(err));
        CHECK(!err.empty());
        CHECK(!src.isConnected());
    }
    {
        FrameSourceSettings s;
        s.visa = true;
        s.visaAddress = std::string(300, 'X'); // 超长地址
        FrameDataSource src(s);
        std::string err;
        CHECK(!src.connect(err));
        CHECK(!err.empty());
    }
}

// ---- 真机资源枚举（需 VISA 运行时；未装/无仪器自动跳过）----

TEST_CASE("VISA 资源枚举（需 NI-VISA/Keysight 等运行时，否则跳过）") {
    std::string err;
    const packet::visa::Api* api = packet::visa::instance(err);
    if (!api) {
        std::printf("    [skip] %s\n", err.c_str());
        return;
    }
    auto resources = packet::visa::listResources(err);
    if (resources.empty()) {
        std::printf("    [skip] 未枚举到 VISA 仪器（%s）\n", err.c_str());
        return;
    }
    // 枚举到的地址须可回填为合法配置（下拉存的地址运行器要能打开）
    for (const auto& r : resources) {
        std::string verr;
        REQUIRE(validateVisaAddress(r, verr));
    }
}
