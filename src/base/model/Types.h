// 基础类型：Id 别名、几何、比较器与告警枚举。
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"

namespace softg {

// ---- Id：工程内由 Project::nextId 单调分配，形如 "comp-12" / "page-1" / "a-5" ----
using ComponentId = std::string;
using PageId = std::string;
using TagName = std::string;
using AssocId = std::string;

// ---- 几何（页面坐标系，左上为原点，单位 px） ----
struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    ImVec2 pos() const { return ImVec2(x, y); }
    ImVec2 size() const { return ImVec2(w, h); }
    ImVec2 center() const { return ImVec2(x + w * 0.5f, y + h * 0.5f); }
    bool contains(ImVec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
};

// ---- 比较器（阈值告警用） ----
enum class Comparator : uint8_t { GT, GE, LT, LE, EQ, NE };

// 判定 actual <cmp> threshold
inline bool compare(Comparator c, double actual, double threshold) {
    switch (c) {
    case Comparator::GT: return actual > threshold;
    case Comparator::GE: return actual >= threshold;
    case Comparator::LT: return actual < threshold;
    case Comparator::LE: return actual <= threshold;
    case Comparator::EQ: return actual == threshold;
    case Comparator::NE: return actual != threshold;
    }
    return false;
}

// ---- 告警严重度 / 视觉样式 ----
enum class AlarmSeverity : uint8_t { Low = 0, High = 1, Critical = 2 };
enum class AlarmStyle : uint8_t { Flash = 0, Border, Color };

} // namespace softg
