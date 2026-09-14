// 单测：画布对齐/尺寸/分布纯几何（AlignOps）
#include "PvTest.h"

#include <vector>

#include "base/model/AlignOps.h"

using namespace pv;

static Rect mk(float x, float y, float w, float h) { return Rect{x, y, w, h}; }

TEST_CASE("对齐：六模式以参考件为基准") {
    Rect ref = mk(100, 50, 40, 20); // 左100 右140 水平中心120；上50 下70 垂直中心60
    Rect a = mk(0, 0, 10, 30);
    Rect b = mk(200, 200, 60, 10);

    { // 左对齐：x == ref.x
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::Left);
        CHECK(p.x == 100);
        CHECK(q.x == 100);
    }
    { // 右对齐：右缘 == ref 右缘
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::Right);
        CHECK(p.x + p.w == 140);
        CHECK(q.x + q.w == 140);
    }
    { // 水平居中：中心 == ref 水平中心
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::HCenter);
        CHECK(p.x + p.w * 0.5f == 120);
        CHECK(q.x + q.w * 0.5f == 120);
    }
    { // 顶对齐
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::Top);
        CHECK(p.y == 50);
        CHECK(q.y == 50);
    }
    { // 底对齐
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::Bottom);
        CHECK(p.y + p.h == 70);
        CHECK(q.y + q.h == 70);
    }
    { // 垂直居中
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        alignRects(rs, ref, AlignMode::VCenter);
        CHECK(p.y + p.h * 0.5f == 60);
        CHECK(q.y + q.h * 0.5f == 60);
    }
}

TEST_CASE("尺寸：等宽/等高/大小相同向基准统一") {
    Rect ref = mk(0, 0, 80, 24);
    Rect a = mk(0, 0, 10, 30), b = mk(0, 0, 50, 5);

    { // 等宽：只改宽
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        sizeRects(rs, ref, SizeMode::Width);
        CHECK(p.w == 80);
        CHECK(q.w == 80);
        CHECK(p.h == 30);
        CHECK(q.h == 5);
    }
    { // 等高：只改高
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        sizeRects(rs, ref, SizeMode::Height);
        CHECK(p.h == 24);
        CHECK(q.h == 24);
        CHECK(p.w == 10);
        CHECK(q.w == 50);
    }
    { // 大小相同：宽高都改
        Rect p = a, q = b;
        std::vector<Rect*> rs{&p, &q};
        sizeRects(rs, ref, SizeMode::Both);
        CHECK(p.w == 80);
        CHECK(p.h == 24);
        CHECK(q.w == 80);
        CHECK(q.h == 24);
    }
}

TEST_CASE("分布：水平/垂直等距且首尾外缘不动") {
    Rect a = mk(0, 0, 10, 10);   // 水平：三个件宽 10/20/10，跨度 0..100
    Rect b = mk(30, 0, 20, 10);
    Rect c = mk(90, 0, 10, 10);
    std::vector<Rect*> rs{&a, &b, &c};
    distributeRects(rs, true);
    CHECK(a.x == 0);              // 首件外缘不动
    CHECK(c.x + c.w == 100);      // 末件外缘不动
    float g1 = b.x - (a.x + a.w);
    float g2 = c.x - (b.x + b.w);
    CHECK(g1 == g2);              // 相邻间距相等
    CHECK(g1 == 30);              // 10+g+20+g+10=100 → g=30
    CHECK(b.x == 40);
    CHECK(c.x == 90);

    Rect p = mk(0, 0, 10, 10);   // 垂直同理
    Rect q = mk(0, 30, 10, 20);
    Rect r = mk(0, 90, 10, 10);
    std::vector<Rect*> vs{&p, &q, &r};
    distributeRects(vs, false);
    CHECK(p.y == 0);
    CHECK(r.y + r.h == 100);
    CHECK(q.y - (p.y + p.h) == r.y - (q.y + q.h));
}

TEST_CASE("对齐/分布：空与单元素边界") {
    Rect ref = mk(0, 0, 1, 1);
    std::vector<Rect*> none;
    alignRects(none, ref, AlignMode::Left); // 不崩溃
    distributeRects(none, true);

    Rect only = mk(5, 6, 7, 8);
    std::vector<Rect*> one{&only};
    alignRects(one, ref, AlignMode::Left);  // 单件仍按基准对齐（调用方保证 ≥2）
    CHECK(only.x == 0);
    CHECK(only.y == 6);                     // 未涉轴不变
    distributeRects(one, true);             // 分布 <3 无操作
    CHECK(only.x == 0);
}
