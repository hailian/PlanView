// M1 单测：领域模型 + JSON 序列化
#include "SoftgTest.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "base/model/ComponentRegistry.h"
#include "base/model/Project.h"
#include "base/model/TagDatabase.h"
#include "base/serialize/ProjectJson.h"

using namespace softg;

// 构造一个覆盖全部 9 种组件 + 三类关联 + 标签的完整工程
static Project makeFullProject() {
    Project p;
    p.name = "测试工程";

    Page page1;
    page1.id = "page-1";
    page1.name = "主画面";
    page1.size = ImVec2(1280, 800);
    int z = 0;
    for (const auto& info : ComponentRegistry::instance().all()) {
        Component c = ComponentRegistry::createComponent(info.typeId, p.allocId("comp"));
        c.name = info.displayName + std::to_string(z);
        c.frame = Rect{10.0f + z * 30.0f, 20.0f + z * 20.0f, info.defaultSize.x, info.defaultSize.y};
        c.z = z++;
        // 每种类型改一个属性值，验证值能往返
        if (!info.properties.empty() && info.typeId == "Gauge")
            c.setProp("value", 66.6);
        if (info.typeId == "Label")
            c.setProp("text", std::string("温度: 中文值"));
        if (info.typeId == "Lamp")
            c.setProp("isOn", true);
        page1.components.push_back(std::move(c));
    }
    page1.sortComponents();

    Page page2;
    page2.id = "page-2";
    page2.name = "副画面";
    page2.background = IM_COL32(40, 20, 20, 255);

    p.pages.push_back(std::move(page1));
    p.pages.push_back(std::move(page2));

    std::string err;
    Tag t1;
    t1.name = "TankLevel";
    t1.type = TagDataType::Float32;
    t1.address = 100;
    t1.scale = 0.01;
    t1.comment = "罐液位";
    (void)p.tags.add(std::move(t1), err);  // fixture 构造，不做断言（宏含 return 不能用于非 void 函数）

    Tag t2;
    t2.name = "PumpOn";
    t2.type = TagDataType::Bool;
    t2.address = 3;
    (void)p.tags.add(std::move(t2), err);

    p.associations.push_back(DataBinding{"a-1", "comp-1", "value", "TankLevel"});
    LinkageRule link;
    link.id = "a-2";
    link.source = "comp-2";
    link.event = LinkageEvent::Click;
    link.action = LinkageAction::Navigate;
    link.param = "page-2";
    p.associations.push_back(std::move(link));

    AlarmRule alarm;
    alarm.id = "a-3";
    alarm.tag = "TankLevel";
    alarm.cmp = Comparator::GE;
    alarm.threshold = 1.4;
    alarm.severity = AlarmSeverity::Critical;
    alarm.style = AlarmStyle::Border;
    alarm.latching = false;
    alarm.components = {"comp-1", "comp-3"};
    p.associations.push_back(std::move(alarm));

    p.nextId = 100;  // 组件/页面 Id 已分配 1..N，预留后续分配起点
    return p;
}

TEST_CASE("JSON 往返字节级一致") {
    Project p = makeFullProject();
    std::string text1 = projio::dump(p);

    Project loaded;
    std::string err;
    REQUIRE(projio::parse(text1, loaded, err));
    std::string text2 = projio::dump(loaded);

    CHECK(text1 == text2);

    // 关键字段抽查
    CHECK(loaded.name == "测试工程");
    CHECK(loaded.pages.size() == 2);
    CHECK(loaded.pages[0].components.size() ==
          ComponentRegistry::instance().all().size());
    CHECK(loaded.tags.all().size() == 2);
    CHECK(loaded.associations.size() == 3);

    const Component* gauge = loaded.pages[0].find("comp-1");
    REQUIRE(gauge != nullptr);
    CHECK(gauge->typeId == "Label");
    CHECK(props::asString(gauge->propOr("text", std::string())) == "温度: 中文值");

    const Component* lamp = loaded.pages[0].find("comp-3");
    REQUIRE(lamp != nullptr);
    CHECK(props::asBool(lamp->propOr("isOn", false)) == true);
}

TEST_CASE("高版本拒载") {
    Project p = makeFullProject();
    std::string text = projio::dump(p);
    // 篡改版本号
    auto pos = text.find("\"schemaVersion\": 1");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 17, "\"schemaVersion\": 9");

    Project loaded;
    std::string err;
    bool ok = projio::parse(text, loaded, err);
    CHECK(!ok);
    CHECK(err.find("版本过高") != std::string::npos);
}

TEST_CASE("文件保存与加载") {
    Project p = makeFullProject();
    std::string err;
    const char* path = "test_roundtrip_tmp.json";
    REQUIRE(projio::save(path, p, err));

    Project loaded;
    REQUIRE(projio::load(path, loaded, err));
    CHECK(projio::dump(loaded) == projio::dump(p));

    REQUIRE(std::remove(path) == 0);
    CHECK(!projio::load("definitely_missing_file.json", loaded, err));
}

TEST_CASE("TagDataType 名称往返") {
    for (uint8_t i = 0; i <= (uint8_t)TagDataType::String; ++i) {
        TagDataType t = (TagDataType)i;
        auto back = tagDataTypeFromName(tagDataTypeName(t));
        REQUIRE(back.has_value());
        CHECK(*back == t);
    }
    CHECK(!tagDataTypeFromName("nope").has_value());
    CHECK(tagDataTypeFromName("float32") == TagDataType::Float32);
}

TEST_CASE("标签库唯一性") {
    TagDatabase db;
    std::string err;
    Tag t;
    t.name = "A";
    t.address = 10;
    CHECK(db.add(t, err));

    Tag dup;
    dup.name = "A";
    dup.address = 20;
    CHECK(!db.add(dup, err));
    CHECK(err.find("重复") != std::string::npos);

    Tag clash;
    clash.name = "B";
    clash.address = 10;
    CHECK(!db.add(clash, err));
    CHECK(err.find("冲突") != std::string::npos);

    // 同槽位不同类型允许（语义由用户保证）
    Tag diffType;
    diffType.name = "C";
    diffType.type = TagDataType::Float32;
    diffType.address = 10;
    CHECK(db.add(diffType, err));

    CHECK(db.all().size() == 2);
    CHECK(db.remove("A"));
    CHECK(!db.remove("A"));
}

TEST_CASE("组件注册表") {
    auto& reg = ComponentRegistry::instance();
    CHECK(reg.all().size() == 13); // 9 基础组件 + 数据源/协议配置（通信）
    REQUIRE(reg.find("Gauge") != nullptr);
    CHECK(reg.find("Gauge")->displayName == "仪表");
    CHECK(reg.find("NoSuchType") == nullptr);
    REQUIRE(reg.find("DataSource") != nullptr);
    CHECK(reg.find("DataSource")->category == "通信");
    REQUIRE(reg.find("ProtocolConfig") != nullptr);
    CHECK(reg.find("ProtocolConfig")->category == "通信");

    Component c = ComponentRegistry::createComponent("Gauge", "comp-x");
    CHECK(c.id == "comp-x");
    CHECK(c.props.count("min") == 1);
    CHECK(props::asDouble(c.propOr("min", -1.0)) == 0.0);
    CHECK(c.frame.w == reg.find("Gauge")->defaultSize.x);
}

TEST_CASE("Project allocId 与查找") {
    Project p;
    Page pg;
    pg.id = "page-1";
    Component c1 = ComponentRegistry::createComponent("Label", p.allocId("comp"));
    Component c2 = ComponentRegistry::createComponent("Lamp", p.allocId("comp"));
    CHECK(c1.id == "comp-1");
    CHECK(c2.id == "comp-2");
    pg.components.push_back(std::move(c1));
    Page pg2;
    pg2.id = "page-2";
    pg2.components.push_back(std::move(c2));
    p.pages.push_back(std::move(pg));
    p.pages.push_back(std::move(pg2));

    CHECK(p.findComponent("comp-2") != nullptr);      // 跨页查找
    CHECK(p.findPageOfComponent("comp-2")->id == "page-2");
    CHECK(p.findComponent("comp-99") == nullptr);
    CHECK(p.nextId == 3);
}
