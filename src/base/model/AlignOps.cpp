#include "base/model/AlignOps.h"

#include <algorithm>

namespace softg {

void alignRects(const std::vector<Rect*>& rects, const Rect& ref, AlignMode mode) {
    switch (mode) {
    case AlignMode::Left:
        for (Rect* r : rects)
            if (r) r->x = ref.x;
        break;
    case AlignMode::Right:
        for (Rect* r : rects)
            if (r) r->x = ref.x + ref.w - r->w;
        break;
    case AlignMode::HCenter: {
        float c = ref.x + ref.w * 0.5f;
        for (Rect* r : rects)
            if (r) r->x = c - r->w * 0.5f;
        break;
    }
    case AlignMode::Top:
        for (Rect* r : rects)
            if (r) r->y = ref.y;
        break;
    case AlignMode::Bottom:
        for (Rect* r : rects)
            if (r) r->y = ref.y + ref.h - r->h;
        break;
    case AlignMode::VCenter: {
        float c = ref.y + ref.h * 0.5f;
        for (Rect* r : rects)
            if (r) r->y = c - r->h * 0.5f;
        break;
    }
    }
}

void sizeRects(const std::vector<Rect*>& rects, const Rect& ref, SizeMode mode) {
    for (Rect* r : rects) {
        if (!r) continue;
        if (mode == SizeMode::Width || mode == SizeMode::Both) r->w = ref.w;
        if (mode == SizeMode::Height || mode == SizeMode::Both) r->h = ref.h;
    }
}

void distributeRects(const std::vector<Rect*>& rects, bool horizontal) {
    std::vector<Rect*> rs;
    rs.reserve(rects.size());
    for (Rect* r : rects)
        if (r) rs.push_back(r);
    if (rs.size() < 3) return;

    // 按主轴起点排序，首尾外缘固定，其余等间距
    std::sort(rs.begin(), rs.end(), [horizontal](const Rect* a, const Rect* b) {
        return horizontal ? a->x < b->x : a->y < b->y;
    });
    auto pos = [horizontal](const Rect* r) { return horizontal ? r->x : r->y; };
    auto size = [horizontal](const Rect* r) { return horizontal ? r->w : r->h; };

    float first = pos(rs.front());
    float last = pos(rs.back()) + size(rs.back());
    float total = 0.0f;
    for (const Rect* r : rs) total += size(r);
    float gap = (last - first - total) / (float)(rs.size() - 1);

    float cursor = first;
    for (Rect* r : rs) {
        if (horizontal)
            r->x = cursor;
        else
            r->y = cursor;
        cursor += size(r) + gap;
    }
}

} // namespace softg
