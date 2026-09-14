#!/usr/bin/env python3
# 生成两款应用的简约风格图标（多尺寸 .ico）。
# 设计语言：深蓝圆角底（与应用面板同族）+ 蓝青双色线性图形；
#   LogicPlanner = 节点连线（组态/关联语义），PageViewer = 屏幕 + 波形（页面/联动刷新）。
# 用法：python tools/make_icons.py  （输出 src/planner/LogicPlanner.ico、src/viewer/PageViewer.ico）
from PIL import Image, ImageDraw

S = 1024            # 绘制画布（4x 超采样后缩到 256，抗锯齿）
OUT = 256           # ico 最大尺寸
BG = (23, 32, 46, 255)       # 深蓝底（应用面板同色系）
BLUE = (77, 141, 232, 255)   # 品牌蓝（数据源→协议曲线）
CYAN = (63, 192, 206, 255)   # 青（字段绑定曲线）
FOG = (150, 170, 198, 255)   # 灰蓝（结构件）
RAD = int(S * 0.23)          # 圆角半径


def canvas():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=RAD, fill=BG)
    return im, d


def line(d, pts, color, w):
    d.line(pts, fill=color, width=w, joint="curve")
    # 端点圆帽（line 默认方头，圆帽更柔和）
    r = w // 2
    for p in (pts[0], pts[-1]):
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=color)


def ring(d, cx, cy, r, color, w):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=color, width=w)


def node(d, cx, cy, r, color, filled):
    if filled:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)
    else:
        ring(d, cx, cy, r, color, int(r * 0.42))


def make_planner():
    im, d = canvas()
    # 三个节点两两连线：左中 → 右上（实心蓝）、右下（空心青），组态/关联拓扑
    a = (S * 0.30, S * 0.64)
    b = (S * 0.70, S * 0.30)
    c = (S * 0.70, S * 0.72)
    lw = int(S * 0.045)
    line(d, [a, b], BLUE, lw)
    line(d, [a, c], CYAN, lw)
    nr = int(S * 0.105)
    node(d, *a, nr, FOG, filled=False)   # 源：空心灰蓝
    node(d, *b, nr, BLUE, filled=True)   # 目标：实心蓝
    node(d, *c, nr, CYAN, filled=False)  # 目标：空心青
    return im


def make_viewer():
    im, d = canvas()
    # 屏幕（圆角描边）+ 屏内波形 + 底座：页面展示 / 数据联动刷新
    x0, y0, x1, y1 = S * 0.16, S * 0.20, S * 0.84, S * 0.68
    d.rounded_rectangle([x0, y0, x1, y1], radius=int(S * 0.055),
                        outline=FOG, width=int(S * 0.042))
    fy = (y0 + y1) / 2
    pts = []
    amp = (y1 - y0) * 0.26
    import math
    n = 48
    for i in range(n + 1):
        t = i / n
        x = x0 + S * 0.075 + t * (S * 0.53)
        y = fy - amp * math.sin(t * math.pi * 2.2) * (1.0 - 0.35 * t)
        pts.append((x, y))
    line(d, pts, CYAN, int(S * 0.042))
    # 底座：短颈 + 横条
    d.rectangle([S * 0.44, y1 + S * 0.042, S * 0.56, y1 + S * 0.095], fill=FOG)
    d.rounded_rectangle([S * 0.33, y1 + S * 0.095, S * 0.67, y1 + S * 0.135],
                        radius=int(S * 0.02), fill=FOG)
    return im


SIZES = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (24, 24), (16, 16)]


def save(im, path):
    im.resize((OUT, OUT), Image.LANCZOS).save(path, format="ICO", sizes=SIZES)
    print("写入", path)


save(make_planner(), "src/planner/LogicPlanner.ico")
save(make_viewer(), "src/viewer/PageViewer.ico")
