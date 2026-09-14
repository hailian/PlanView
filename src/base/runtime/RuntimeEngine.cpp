#include "base/runtime/RuntimeEngine.h"

#include "base/log/Log.h"
#include "base/model/ComponentRegistry.h"
#include "base/serialize/JsonHelpers.h"

#include <algorithm>

namespace pv {

namespace {
// PropertyValue -> TagValue（Color/Vec2 不适用写标签，退化为字符串）
TagValue propertyToTagValue(const PropertyValue& v) {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* i = std::get_if<int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    return props::asString(v);
}
} // namespace

// ---- 数据入口 ----

void RuntimeEngine::applyTagUpdates(const std::vector<TagReadResult>& updates) {
    for (const TagReadResult& u : updates) {
        Tag* t = p_->tags.find(u.tag);
        if (!t) continue;

        if (u.ok) {
            t->currentValue = u.value;
            t->quality = TagQuality::Good;
        } else {
            t->quality = (u.quality == TagQuality::CommLost) ? TagQuality::CommLost
                                                             : TagQuality::Bad;
        }
        t->lastUpdate = now_;

        if (u.ok) {
            // 曲线采样
            auto& hist = history_[u.tag];
            hist.push_back({now_, (float)t->numeric()});
            while (hist.size() > kHistoryCap) hist.pop_front();

            onTagChanged(u.tag, 0);
        }
    }
}

void RuntimeEngine::onTagChanged(const TagName& tag, int depth) {
    if (depth > kMaxDepth) {
        PV_LOG_WARN("联动链超过 %d 层，截断 (标签 %s)", kMaxDepth, tag.c_str());
        return;
    }
    const Tag* t = p_->tags.find(tag);
    if (!t) return;

    // 1) 刷新绑定覆盖层 + 值变化级联
    for (const auto& a : p_->associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (!b || b->tag != tag) continue;
        const Component* comp = p_->findComponent(b->component);
        bound_[{b->component, b->property}] = tagToPropertyValue(t->currentValue, comp, b->property);
        for (const auto& la : p_->associations) {
            auto* l = std::get_if<LinkageRule>(&la);
            if (l && l->source == b->component && l->event == LinkageEvent::ValueChanged)
                executeAction(*l, depth + 1);
        }
    }

    // 2) 告警评估
    for (const auto& a : p_->associations) {
        if (auto* r = std::get_if<AlarmRule>(&a); r && r->tag == tag)
            evaluateAlarmRule(*r, depth + 1);
    }
}

void RuntimeEngine::evaluateAlarmRule(const AlarmRule& rule, int depth) {
    const Tag* t = p_->tags.find(rule.tag);
    if (!t) return;

    AlarmState& st = alarmStates_[rule.id];
    bool condition =
        t->quality == TagQuality::Good && compare(rule.cmp, t->numeric(), rule.threshold);

    if (condition && !st.active) {
        st.active = true;
        if (rule.latching) st.latched = true;
        if (std::find(firedRules_.begin(), firedRules_.end(), rule.id) == firedRules_.end())
            firedRules_.push_back(rule.id);
        PV_LOG_INFO("告警触发: %s %s %.3g (%s)", rule.tag.c_str(),
                       jsonx::comparatorToString(rule.cmp).c_str(), rule.threshold,
                       rule.id.c_str());
        for (const auto& cid : alarmAffectedComponents(rule))
            for (const auto& a : p_->associations) {
                auto* l = std::get_if<LinkageRule>(&a);
                if (l && l->source == cid && l->event == LinkageEvent::AlarmActive)
                    executeAction(*l, depth);
            }
    } else if (!condition && st.active && !st.latched) {
        st.active = false;
        firedRules_.erase(std::remove(firedRules_.begin(), firedRules_.end(), rule.id),
                          firedRules_.end());
        for (const auto& cid : alarmAffectedComponents(rule))
            for (const auto& a : p_->associations) {
                auto* l = std::get_if<LinkageRule>(&a);
                if (l && l->source == cid && l->event == LinkageEvent::AlarmCleared)
                    executeAction(*l, depth);
            }
    }
}

void RuntimeEngine::raiseComponentEvent(const ComponentId& id, LinkageEvent ev) {
    for (const auto& a : p_->associations) {
        auto* l = std::get_if<LinkageRule>(&a);
        if (l && l->source == id && l->event == ev)
            executeAction(*l, 0);
    }
}

void RuntimeEngine::setInteractiveValue(const ComponentId& id, const std::string& key, TagValue v) {
    Component* comp = p_->findComponent(id);
    if (!comp) return;
    PropertyValue pv = tagToPropertyValue(v, comp, key);
    setValueProperty(id, key, pv, 0, true);
    // 有绑定时排队写回（服务器刷新后经 applyTagUpdates 再校正显示）
    for (const auto& a : p_->associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (b && b->component == id && b->property == key) {
            PendingAction act;
            act.kind = PendingAction::Kind::WriteTag;
            act.target = b->tag;
            act.value = v;
            actions_.push_back(std::move(act));
            break;
        }
    }
}

void RuntimeEngine::setValueProperty(const ComponentId& compId, const std::string& key,
                                     PropertyValue v, int depth, bool fireValueChanged) {
    Component* comp = p_->findComponent(compId);
    if (!comp) return;

    // 该属性是否存在数据绑定（有则同步覆盖层，显示立即反映）
    auto boundKey = std::make_pair(compId, key);
    bool isBoundKey = false;
    for (const auto& a : p_->associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (b && b->component == compId && b->property == key) {
            isBoundKey = true;
            break;
        }
    }

    if (key == "visible") {
        comp->visible = props::asBool(v, comp->visible);
    } else {
        comp->setProp(key, v);
        if (isBoundKey) bound_[boundKey] = v;
    }

    if (fireValueChanged) {
        if (depth > kMaxDepth) {
            PV_LOG_WARN("联动链超过 %d 层，截断 (组件 %s)", kMaxDepth, compId.c_str());
            return;
        }
        for (const auto& a : p_->associations) {
            auto* l = std::get_if<LinkageRule>(&a);
            if (l && l->source == compId && l->event == LinkageEvent::ValueChanged)
                executeAction(*l, depth + 1);
        }
    }
}

void RuntimeEngine::executeAction(const LinkageRule& rule, int depth) {
    if (depth > kMaxDepth) {
        PV_LOG_WARN("联动链超过 %d 层，截断 (%s)", kMaxDepth, rule.id.c_str());
        return;
    }
    switch (rule.action) {
    case LinkageAction::SetProperty:
        setValueProperty(rule.target, rule.param, rule.value, depth, true);
        break;
    case LinkageAction::Navigate: {
        PendingAction a;
        a.kind = PendingAction::Kind::Navigate;
        a.target = rule.param;
        actions_.push_back(std::move(a));
        break;
    }
    case LinkageAction::ToggleVisible: {
        if (Component* c = p_->findComponent(rule.target)) c->visible = !c->visible;
        break;
    }
    case LinkageAction::SetTagValue: {
        PendingAction a;
        a.kind = PendingAction::Kind::WriteTag;
        a.target = rule.param;
        a.value = propertyToTagValue(rule.value);
        actions_.push_back(std::move(a));
        break;
    }
    case LinkageAction::Pulse:
        pulseUntil_[rule.target] = now_ + std::chrono::milliseconds(400);
        break;
    }
}

std::vector<PendingAction> RuntimeEngine::drainActions() {
    std::vector<PendingAction> out;
    out.swap(actions_);
    return out;
}

// ---- 告警确认 ----

void RuntimeEngine::acknowledgeAlarm(const AssocId& ruleId) {
    auto it = alarmStates_.find(ruleId);
    if (it == alarmStates_.end()) return;
    const AlarmRule* rule = findRule(ruleId);
    bool condition = false;
    if (rule) {
        if (const Tag* t = p_->tags.find(rule->tag))
            condition =
                t->quality == TagQuality::Good && compare(rule->cmp, t->numeric(), rule->threshold);
    }
    if (!condition) {
        it->second.active = false;
        it->second.latched = false;
        firedRules_.erase(std::remove(firedRules_.begin(), firedRules_.end(), ruleId),
                          firedRules_.end());
    } else {
        it->second.latched = false;  // 条件仍满足：保持激活但不再是锁存态
    }
}

void RuntimeEngine::acknowledgeAllAlarms() {
    for (const AssocId& id : firedRules_) acknowledgeAlarm(id);
}

const AlarmRule* RuntimeEngine::findRule(const AssocId& id) const {
    for (const auto& a : p_->associations)
        if (auto* r = std::get_if<AlarmRule>(&a); r && r->id == id) return r;
    return nullptr;
}

int RuntimeEngine::goodTagCount() const {
    int n = 0;
    for (const auto& t : p_->tags.all())
        if (t.quality == TagQuality::Good) ++n;
    return n;
}

// ---- Provider 实现 ----

const PropertyValue* RuntimeEngine::resolved(const ComponentId& id, std::string_view key) const {
    auto it = bound_.find(std::make_pair(id, std::string(key)));
    return it != bound_.end() ? &it->second : nullptr;
}

void RuntimeEngine::getSeries(const ComponentId& compId, std::vector<ChartPoint>& out) const {
    out.clear();
    TagName tag;
    bool bound = false;
    for (const auto& a : p_->associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (b && b->component == compId && b->property == "value") {
            tag = b->tag;
            bound = true;
            break;
        }
    }
    if (bound) {
        auto it = history_.find(tag);
        if (it != history_.end()) {
            out.assign(it->second.begin(), it->second.end());
            return;
        }
    }
    // 未绑定：以组件本地 value 画平线（两点足够）
    const Component* c = p_->findComponent(compId);
    if (c) {
        float v = (float)props::asDouble(c->propOr("value", 0.0));
        out.push_back({now_ - std::chrono::seconds(60), v});
        out.push_back({now_, v});
    }
}

bool RuntimeEngine::alarmOf(const ComponentId& id, AlarmVisual& out) const {
    // 脉冲高亮优先（短促黄框）
    auto pit = pulseUntil_.find(id);
    if (pit != pulseUntil_.end() && pit->second > now_) {
        out = {AlarmSeverity::Low, AlarmStyle::Border};
        return true;
    }
    AlarmVisual best{};
    bool any = false;
    for (const auto& a : p_->associations) {
        auto* r = std::get_if<AlarmRule>(&a);
        if (!r) continue;
        auto sit = alarmStates_.find(r->id);
        if (sit == alarmStates_.end() || !sit->second.active) continue;
        bool affected = false;
        for (const auto& cid : r->components)
            if (cid == id) { affected = true; break; }
        if (!affected) {
            for (const auto& ba : p_->associations) {
                auto* b = std::get_if<DataBinding>(&ba);
                if (b && b->component == id && b->tag == r->tag) { affected = true; break; }
            }
        }
        if (affected) {
            if (!any || (int)r->severity > (int)best.severity) {
                best = {r->severity, r->style};
                any = true;
            }
        }
    }
    if (any) out = best;
    return any;
}

// ---- 内部 ----

std::vector<ComponentId> RuntimeEngine::alarmAffectedComponents(const AlarmRule& rule) const {
    std::vector<ComponentId> comps = rule.components;
    for (const auto& a : p_->associations) {
        auto* b = std::get_if<DataBinding>(&a);
        if (b && b->tag == rule.tag)
            comps.push_back(b->component);
    }
    return comps;
}

PropertyValue RuntimeEngine::tagToPropertyValue(const TagValue& v, const Component* target,
                                                 const std::string& key) const {
    // 按目标属性规格类型转换：Bool → 布尔化；String → 文本化；其余保持数值型别
    PropertyType specType = PropertyType::Bool;
    bool hasSpec = false;
    if (target) {
        if (const auto* info = ComponentRegistry::instance().find(target->typeId)) {
            for (const auto& spec : info->properties) {
                if (spec.key == key) {
                    specType = spec.type;
                    hasSpec = true;
                    break;
                }
            }
        }
    }

    auto toText = [](const TagValue& val) -> std::string {
        char buf[64];
        if (auto* b = std::get_if<bool>(&val)) return *b ? "true" : "false";
        if (auto* i = std::get_if<int64_t>(&val)) {
            snprintf(buf, sizeof(buf), "%lld", (long long)*i);
            return buf;
        }
        if (auto* d = std::get_if<double>(&val)) {
            snprintf(buf, sizeof(buf), "%.4g", *d);
            return buf;
        }
        return std::get<std::string>(val);
    };

    if (hasSpec && specType == PropertyType::String) return toText(v);
    if (hasSpec && specType == PropertyType::Bool) {
        if (auto* b = std::get_if<bool>(&v)) return *b;
        if (auto* i = std::get_if<int64_t>(&v)) return *i != 0;
        if (auto* d = std::get_if<double>(&v)) return *d != 0.0;
    }
    if (auto* b = std::get_if<bool>(&v)) return *b ? 1 : 0;
    if (auto* i = std::get_if<int64_t>(&v)) return *i;
    if (auto* d = std::get_if<double>(&v)) return *d;
    return std::get<std::string>(v);
}

} // namespace pv
