// 全组件单测：注册表 13 类组件的规格健全性、创建默认值、属性读写与值语义。
// 新增组件类型时应同步登记 kTypes（防漏配默认属性/规格）。
#include "PvTest.h"

#include <set>

#include "base/model/ComponentRegistry.h"

using namespace pv;

namespace {

// 内置类型全集（显示/操作/容器/图形/通信）
constexpr const char* kTypes[] = {
    "Label", "Button", "Lamp", "Gauge", "Chart", "Switch", "Slider",
    "Panel", "Image", "DataSource", "ProtocolGroup", "ProtocolConfig", "DataSink",
};

} // namespace

TEST_CASE("组件注册表：全类型规格健全性") {
    auto& reg = ComponentRegistry::instance();
    REQUIRE(reg.all().size() == sizeof(kTypes) / sizeof(kTypes[0]));
    std::set<std::string> typeIds;
    for (const auto& t : reg.all()) {
        CHECK(!t.typeId.empty());
        CHECK(!t.displayName.empty());
        CHECK(!t.category.empty());
        CHECK(t.defaultSize.x > 0 && t.defaultSize.y > 0);
        CHECK(typeIds.insert(t.typeId).second); // typeId 不得重复
        std::set<std::string> keys;
        for (const auto& s : t.properties) {
            CHECK(!s.key.empty());
            CHECK(!s.label.empty());
            CHECK(keys.insert(s.key).second); // 属性键不得重复
            if (s.type == PropertyType::Enum) CHECK(!s.enumValues.empty());
            if (s.minValue && s.maxValue) CHECK(*s.minValue <= *s.maxValue);
        }
    }
    for (const char* ty : kTypes) CHECK(reg.find(ty) != nullptr);
    CHECK(reg.find("NoSuchType") == nullptr);
}

TEST_CASE("组件创建：默认属性与规格一致 + 值语义拷贝独立") {
    auto& reg = ComponentRegistry::instance();
    for (const char* ty : kTypes) {
        const ComponentTypeInfo* info = reg.find(ty);
        REQUIRE(info != nullptr);
        // 创建即带全部默认属性，几何取注册表默认尺寸
        Component c = ComponentRegistry::createComponent(ty, "cid-x");
        CHECK(c.typeId == ty);
        CHECK(c.id == "cid-x");
        CHECK(c.visible);
        CHECK(c.frame.w == (int)info->defaultSize.x);
        CHECK(c.frame.h == (int)info->defaultSize.y);
        REQUIRE(c.props.size() == info->properties.size());
        for (const auto& s : info->properties)
            CHECK(c.props.count(s.key) == 1); // 默认值逐规格就位

        // 属性读写往返：取首个规格键，写入新值后 propOr 读回
        if (!info->properties.empty()) {
            const std::string key = info->properties.front().key;
            c.setProp(key, std::string("往返"));
            CHECK(props::asString(c.propOr(key, std::string())) == "往返");
        }

        // 值语义：拷贝即快照，副本改属性不影响原件（undo/redo 依赖）
        Component copy = c;
        copy.setProp("note-x", int64_t(7));
        CHECK(copy.props.count("note-x") == 1);
        CHECK(c.props.count("note-x") == 0);
    }
}

TEST_CASE("通信组件关键默认值（新工程拖入即用）") {
    // 数据源：UDP + 服务端角色 + 手动启动 + 9001
    Component ds = ComponentRegistry::createComponent("DataSource", "ds");
    CHECK(props::asString(ds.propOr("transport", std::string())) == "UDP");
    CHECK(props::asString(ds.propOr("udpRole", std::string())) == "服务端");
    CHECK(props::asBool(ds.propOr("autoStart", true)) == false);
    CHECK(props::asInt(ds.propOr("localPort", int64_t(0))) == 9001);
    CHECK(props::asString(ds.propOr("host", std::string())) == "127.0.0.1");
    CHECK(props::asString(ds.propOr("listenMode", std::string())) == "本机端口");

    // 数据目的：TCP + 客户端角色 + 9002
    Component sk = ComponentRegistry::createComponent("DataSink", "sk");
    CHECK(props::asString(sk.propOr("transport", std::string())) == "TCP");
    CHECK(props::asString(sk.propOr("tcpRole", std::string())) == "客户端");
    CHECK(props::asInt(sk.propOr("remotePort", int64_t(0))) == 9002);
    CHECK(props::asString(sk.propOr("source", std::string("<未设>"))) == "");

    // 协议配置：TLV + 无字段（字段为 Inspector 动态写的 f<i>.* 索引属性，不在规格）
    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "pr");
    CHECK(props::asString(proto.propOr("framingMode", std::string())) == "TLV");
    CHECK(proto.props.count("f0.name") == 0);

    // 协议组：无成员（成员为 p<i>.* 索引属性）
    Component grp = ComponentRegistry::createComponent("ProtocolGroup", "gp");
    CHECK(grp.props.count("p0.name") == 0);
}

TEST_CASE("显示/操作组件代表属性默认值") {
    Component gauge = ComponentRegistry::createComponent("Gauge", "g");
    CHECK(props::asDouble(gauge.propOr("min", -1.0)) == 0.0);
    Component lamp = ComponentRegistry::createComponent("Lamp", "l");
    CHECK(props::asBool(lamp.propOr("isOn", true)) == false);
    Component sw = ComponentRegistry::createComponent("Switch", "s");
    CHECK(props::asBool(sw.propOr("isOn", true)) == false);
    Component label = ComponentRegistry::createComponent("Label", "t");
    CHECK(props::asString(label.propOr("text", std::string("<缺>"))) != "<缺>");
    Component chart = ComponentRegistry::createComponent("Chart", "c");
    CHECK(props::asInt(chart.propOr("spanSec", int64_t(0))) == 60);
    CHECK(props::asDouble(chart.propOr("max", -1.0)) == 100.0);
}
