// M6 单测：RuntimeEngine 三语义（绑定刷新 / 联动 / 告警锁存确认 / 写回排队）
#include "SoftgTest.h"

#include "base/model/ComponentRegistry.h"
#include "base/runtime/RuntimeEngine.h"

using namespace softg;

// 构造运行时测试工程：
//   comp-g(Gauge, value) 绑定 tagTemp；comp-l(Lamp, isOn)；comp-b(Button)
//   绑定1: Gauge.value <- tagTemp
//   联动1: Button Click -> Lamp.setProperty isOn=true
//   联动2: Lamp ValueChanged -> Lamp2.setProperty isOn=!... (链)
//   告警1: tagTemp > 80 -> severity High, latching, 覆盖 comp-g
static Project makeRuntimeProject() {
    Project p;
    p.pages.push_back(Page{});
    p.pages.back().id = "page-1";
    Page& page = p.pages.back();

    Component gauge = ComponentRegistry::createComponent("Gauge", "comp-g");
    gauge.setProp("min", 0.0);
    gauge.setProp("max", 200.0);
    Component lamp = ComponentRegistry::createComponent("Lamp", "comp-l");
    Component lamp2 = ComponentRegistry::createComponent("Lamp", "comp-l2");
    Component button = ComponentRegistry::createComponent("Button", "comp-b");
    page.components = {gauge, lamp, lamp2, button};

    Tag t;
    t.name = "tagTemp";
    t.type = TagDataType::Float32;
    t.address = 0;
    std::string err;
    (void)p.tags.add(t, err);

    p.associations.push_back(DataBinding{"a-bind", "comp-g", "value", "tagTemp"});
    LinkageRule l1;
    l1.id = "a-l1";
    l1.source = "comp-b";
    l1.event = LinkageEvent::Click;
    l1.target = "comp-l";
    l1.action = LinkageAction::SetProperty;
    l1.param = "isOn";
    l1.value = true;
    p.associations.push_back(std::move(l1));
    LinkageRule l2;
    l2.id = "a-l2";
    l2.source = "comp-l";
    l2.event = LinkageEvent::ValueChanged;
    l2.target = "comp-l2";
    l2.action = LinkageAction::SetProperty;
    l2.param = "isOn";
    l2.value = true;
    p.associations.push_back(std::move(l2));
    LinkageRule nav;
    nav.id = "a-nav";
    nav.source = "comp-b";
    nav.event = LinkageEvent::Click;
    nav.action = LinkageAction::Navigate;
    nav.param = "page-2";
    p.associations.push_back(std::move(nav));
    AlarmRule alarm;
    alarm.id = "a-alm";
    alarm.tag = "tagTemp";
    alarm.cmp = Comparator::GT;
    alarm.threshold = 80.0;
    alarm.severity = AlarmSeverity::High;
    alarm.style = AlarmStyle::Flash;
    alarm.latching = true;
    p.associations.push_back(std::move(alarm));
    return p;
}

static TagReadResult update(const char* tag, double v) {
    TagReadResult r;
    r.tag = tag;
    r.ok = true;
    r.quality = TagQuality::Good;
    r.value = v;
    return r;
}

TEST_CASE("数据绑定刷新与曲线采样") {
    Project p = makeRuntimeProject();
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());

    engine.applyTagUpdates({update("tagTemp", 42.5)});
    const PropertyValue* v = engine.resolved("comp-g", "value");
    REQUIRE(v != nullptr);
    CHECK(props::asDouble(*v) == 42.5);

    std::vector<ChartPoint> series;
    engine.getSeries("comp-g", series);
    CHECK(series.size() == 1);
    engine.applyTagUpdates({update("tagTemp", 50.0)});
    engine.getSeries("comp-g", series);
    CHECK(series.size() == 2);
    CHECK(series[1].v == 50.0f);
}

TEST_CASE("联动: 点击->设置属性 + 值变化级联 + 导航动作") {
    Project p = makeRuntimeProject();
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());

    engine.raiseComponentEvent("comp-b", LinkageEvent::Click);
    // l1: comp-l.isOn = true；l2: comp-l 值变化 -> comp-l2.isOn = true
    CHECK(props::asBool(p.findComponent("comp-l")->propOr("isOn", false)) == true);
    CHECK(props::asBool(p.findComponent("comp-l2")->propOr("isOn", false)) == true);

    auto actions = engine.drainActions();
    bool hasNav = false;
    for (const auto& a : actions)
        if (a.kind == PendingAction::Kind::Navigate && a.target == "page-2") hasNav = true;
    CHECK(hasNav);
}

TEST_CASE("交互写值与绑定写回排队") {
    Project p = makeRuntimeProject();
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());

    // comp-g 的 value 有绑定：写交互值应排队 WriteTag
    engine.setInteractiveValue("comp-g", "value", TagValue(120.0));
    const PropertyValue* v = engine.resolved("comp-g", "value");
    REQUIRE(v != nullptr);
    CHECK(props::asDouble(*v) == 120.0);

    auto actions = engine.drainActions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == PendingAction::Kind::WriteTag);
    CHECK(actions[0].target == "tagTemp");
    CHECK(std::get<double>(actions[0].value) == 120.0);
}

TEST_CASE("告警: 触发/锁存/自动覆盖绑定组件/确认") {
    Project p = makeRuntimeProject();
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());

    // 未越限: 无告警
    engine.applyTagUpdates({update("tagTemp", 50.0)});
    CHECK(engine.activeAlarmCount() == 0);
    AlarmVisual av;
    CHECK(!engine.alarmOf("comp-g", av));

    // 越限: 触发（comp-g 绑定了 tagTemp，自动覆盖）
    engine.applyTagUpdates({update("tagTemp", 95.0)});
    CHECK(engine.activeAlarmCount() == 1);
    CHECK(engine.alarmOf("comp-g", av));
    CHECK(av.severity == AlarmSeverity::High);
    CHECK(av.style == AlarmStyle::Flash);

    // 恢复正常但锁存: 保持激活
    engine.applyTagUpdates({update("tagTemp", 30.0)});
    CHECK(engine.activeAlarmCount() == 1);

    // 确认: 条件已不满足 -> 清除
    engine.acknowledgeAlarm("a-alm");
    CHECK(engine.activeAlarmCount() == 0);
    CHECK(!engine.alarmOf("comp-g", av));
}

TEST_CASE("告警联动事件") {
    Project p = makeRuntimeProject();
    // 追加: 告警触发时翻转 comp-l2 可见性
    LinkageRule onAlarm;
    onAlarm.id = "a-l3";
    onAlarm.source = "comp-g";
    onAlarm.event = LinkageEvent::AlarmActive;
    onAlarm.target = "comp-l2";
    onAlarm.action = LinkageAction::ToggleVisible;
    p.associations.push_back(std::move(onAlarm));

    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());
    bool visBefore = p.findComponent("comp-l2")->visible;
    engine.applyTagUpdates({update("tagTemp", 95.0)});
    CHECK(p.findComponent("comp-l2")->visible == !visBefore);
}

TEST_CASE("联动环截断") {
    Project p;
    p.pages.push_back(Page{});
    p.pages.back().id = "page-1";
    p.pages.back().components.push_back(ComponentRegistry::createComponent("Lamp", "comp-a"));
    p.pages.back().components.push_back(ComponentRegistry::createComponent("Lamp", "comp-b"));
    // a 值变化 -> b 设属性(触发 b 值变化) -> a 设属性 -> ... 环
    for (int i = 0; i < 2; ++i) {
        LinkageRule l;
        l.id = i == 0 ? "loop1" : "loop2";
        l.source = i == 0 ? "comp-a" : "comp-b";
        l.event = LinkageEvent::ValueChanged;
        l.target = i == 0 ? "comp-b" : "comp-a";
        l.action = LinkageAction::SetProperty;
        l.param = "isOn";
        l.value = true;
        p.associations.push_back(std::move(l));
    }
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());
    // 不死锁即通过；深度上限截断
    engine.setInteractiveValue("comp-a", "isOn", TagValue(true));
    CHECK(props::asBool(p.findComponent("comp-a")->propOr("isOn", false)) == true);
    CHECK(props::asBool(p.findComponent("comp-b")->propOr("isOn", false)) == true);
}

TEST_CASE("非锁存告警自动恢复") {
    Project p = makeRuntimeProject();
    // 改为非锁存
    for (auto& a : p.associations)
        if (auto* r = std::get_if<AlarmRule>(&a); r && r->id == "a-alm") r->latching = false;

    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());
    engine.applyTagUpdates({update("tagTemp", 95.0)});
    CHECK(engine.activeAlarmCount() == 1);
    engine.applyTagUpdates({update("tagTemp", 50.0)});
    CHECK(engine.activeAlarmCount() == 0);  // 自动恢复，无需确认
}

TEST_CASE("数据质量: 通讯丢失不触发告警") {
    Project p = makeRuntimeProject();
    RuntimeEngine engine(p);
    engine.tick(std::chrono::steady_clock::now());
    engine.applyTagUpdates({update("tagTemp", 95.0)});
    CHECK(engine.activeAlarmCount() == 1);

    TagReadResult lost;
    lost.tag = "tagTemp";
    lost.ok = false;
    lost.quality = TagQuality::CommLost;
    engine.applyTagUpdates({lost});
    CHECK(engine.activeAlarmCount() == 1);  // 锁存保持；数据坏不触发新告警
    CHECK(engine.goodTagCount() == 0);
}
