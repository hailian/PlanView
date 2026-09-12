// RuntimeEngine — 运行器规则求值中枢：
//   数据绑定刷新 / 组件联动执行(环防护) / 阈值告警评估(锁存/确认) / 曲线历史 / 动作队列。
// 仅在 UI 线程调用（worker 线程的数据经 PollWorker 队列转入）。
#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/data/IDataSource.h"  // TagReadResult
#include "base/model/Project.h"
#include "base/render/RenderContext.h"

namespace softg {

// UI 待消费动作（导航 / 写标签回传）
struct PendingAction {
    enum class Kind : uint8_t { Navigate, WriteTag } kind = Kind::Navigate;
    std::string target;  // Navigate: PageId; WriteTag: TagName
    TagValue value{};    // WriteTag 的工程值
};

class RuntimeEngine : public IPropertyProvider,
                      public IChartSeriesSource,
                      public IAlarmSource {
public:
    explicit RuntimeEngine(Project& project) : p_(&project) {}

    // 重新绑定工程（加载新工程时；清空全部运行时状态）
    void rebind(Project& project) {
        p_ = &project;
        bound_.clear();
        history_.clear();
        alarmStates_.clear();
        firedRules_.clear();
        pulseUntil_.clear();
        actions_.clear();
        now_ = {};
    }

    // ---- 数据入口（UI 线程） ----
    // 应用一轮轮询结果：写标签值 → 刷新绑定覆盖层 → 采样曲线 → 告警评估 → 级联值变化联动
    void applyTagUpdates(const std::vector<TagReadResult>& updates);
    // 组件交互事件（点击/值变化等，来自 viewer）
    void raiseComponentEvent(const ComponentId& id, LinkageEvent ev);
    // 交互组件写值（Switch/Slider 等）：立即反映显示 + 触发联动 + 有绑定时排队写回
    void setInteractiveValue(const ComponentId& id, const std::string& key, TagValue v);
    // 每帧调用：维护时钟（脉冲超时等）
    void tick(std::chrono::steady_clock::time_point now) { now_ = now; }
    // 取走并清空动作队列（viewer 消费：Navigate 切页 / WriteTag 入 worker 写队列）
    std::vector<PendingAction> drainActions();

    // ---- 告警 ----
    void acknowledgeAlarm(const AssocId& ruleId);  // 确认锁存告警
    void acknowledgeAllAlarms();
    const std::vector<AssocId>& firedAlarmRules() const { return firedRules_; }
    int activeAlarmCount() const { return (int)firedRules_.size(); }
    const AlarmRule* findRule(const AssocId& id) const;

    // ---- Provider 实现（渲染上下文注入） ----
    const PropertyValue* resolved(const ComponentId& id, std::string_view key) const override;
    void getSeries(const ComponentId& compId, std::vector<ChartPoint>& out) const override;
    bool alarmOf(const ComponentId& id, AlarmVisual& out) const override;

    // 查询辅助（详情弹窗/状态栏）
    const Project& project() const { return *p_; }
    int goodTagCount() const;
    const Tag* findTag(const TagName& name) const { return p_->tags.find(name); }

    static constexpr int kMaxDepth = 8;          // 联动环防护
    static constexpr size_t kHistoryCap = 512;   // 每标签曲线样本上限

private:
    // 联动执行（depth 递增防环）
    void executeAction(const LinkageRule& rule, int depth);
    // 写组件属性（SetProperty 联动 / 交互写值共用）：本地 + 绑定覆盖层 + 值变化级联
    void setValueProperty(const ComponentId& comp, const std::string& key, PropertyValue v,
                          int depth, bool fireValueChanged);
    // 标签值变化 -> 告警评估 + 该标签绑定组件的值变化级联
    void onTagChanged(const TagName& tag, int depth);
    void evaluateAlarmRule(const AlarmRule& rule, int depth);
    // 规则覆盖的组件集（显式列表 ∪ 绑定该标签的组件）
    std::vector<ComponentId> alarmAffectedComponents(const AlarmRule& rule) const;

    PropertyValue tagToPropertyValue(const TagValue& v, const Component* target,
                                     const std::string& key) const;

    Project* p_;
    std::chrono::steady_clock::time_point now_{};

    // 绑定覆盖层：绑定属性的最新值（优先于组件本地属性显示）
    std::map<std::pair<ComponentId, std::string>, PropertyValue> bound_;
    // 每标签曲线环形历史
    std::map<TagName, std::deque<ChartPoint>> history_;
    // 告警规则状态
    struct AlarmState {
        bool active = false;
        bool latched = false;
    };
    std::map<AssocId, AlarmState> alarmStates_;
    std::vector<AssocId> firedRules_;  // 激活(含锁存)规则，按激活时间序
    // 脉冲高亮到期表
    std::map<ComponentId, std::chrono::steady_clock::time_point> pulseUntil_;
    std::vector<PendingAction> actions_;
};

} // namespace softg
