# SoftG — HMI 组态软件（逻辑规划 + 页面展示）

Windows / C++20 / Dear ImGui 组态软件套件，分三部分：

| 部分 | 目标 | 说明 |
|---|---|---|
| **softg_base** | 静态库 | 两款软件共用：组件模型 / 关联模型 / TCP 数据层 / JSON 序列化 / **共享组件渲染器** / 运行引擎 |
| **LogicPlanner** | exe | 逻辑规划软件（设计器）：拖拽组件布局页面 → 编辑数据绑定/联动/告警 → 生成 `.json` 配置 |
| **PageViewer** | exe | 页面展示软件（运行器）：打开配置 → 按配置渲染页面 → TCP 数据联动刷新 |

核心设计：**组件绘制代码只存在于 base 的 ComponentRenderer 中**，设计器画布与运行器渲染调用同一函数——"所见即所得"由链接期保证。

## 构建

需要 Visual Studio 2022+（含 C++ 桌面开发与 CMake 组件）。两种方式：

```bat
:: 命令行（推荐）
scripts\build.cmd x64-debug      :: 或 x64-release

:: Visual Studio：文件→打开→CMake...，选择仓库根目录，选 x64-debug/x64-release 预设
```

依赖经 CMake FetchContent 自动拉取（默认 gitee 镜像，网络环境不同可用 `-D SOFTG_IMGUI_REPO=...` 覆盖）：
- Dear ImGui `v1.92.9b-docking`（Win32 + DX11 后端）
- nlohmann/json `v3.12.0`

**DPI 缩放**：界面按显示器 DPI 自动缩放（字体/间距/窗口尺寸/画布默认缩放），跨不同缩放比例的显示器拖动时热切换；环境变量 `SOFTG_UI_SCALE`（如 `set SOFTG_UI_SCALE=1.5`）可强制指定缩放。

构建产物（所有 exe 直接位于 preset 目录下）：`out/build/<preset>/LogicPlanner.exe`、`out/build/<preset>/PageViewer.exe`、`out/build/<preset>/softg_tests.exe`。

## 使用

### 端到端演示

```bat
:: 1) 启动数据服务器模拟器（槽位 0-5 正弦波 / 6 随机游走 / 7 方波 / 8 计数器）
python tools\tcp_sim.py 9000

:: 2) 用运行器打开演示工程
out\build\x64-debug\PageViewer.exe examples\demo_project.json
```

演示内容：仪表/曲线随模拟数据刷新；「启动/停止」按钮写回槽位 7（指示灯与开关同步）；温度 1 > 25 触发闪烁告警（顶部告警条可确认）；液位 ≥ 35 触发描边告警（非锁存，自动恢复）；「副画面」按钮页面跳转；滑块拖动写回槽位 6（模拟器控制台可见 WRITE）。

### 设计器（LogicPlanner）

- **组件面板** 拖到画布创建（双击添加到左上角）；画布滚轮缩放、中键平移、框选/Ctrl 多选、8 手柄缩放、网格吸附
- **对齐工具栏**（画布顶部）：多选后左/右/水平居中、顶/底/垂直居中、等宽/等高/大小相同、水平/垂直等距分布；对齐与尺寸以**最后点击的组件（锚点，角上琥珀圆点）**为基准
- **属性** 面板编辑选中组件的类型化属性 / 几何 / 层级
- **标签库** 定义数据点（槽位索引 + 类型 + scale/offset）
- **关联关系** 面板创建/编辑三类关联：数据绑定（组件属性←标签）、组件联动（事件→动作）、阈值告警
- **校验** 面板扫描悬空引用；Ctrl+Z/Y 撤销重做，Ctrl+C/V/D 复制粘贴再制
- 文件→保存/另存为 生成 `.json` 配置

### 运行器（PageViewer）

- 打开工程（或命令行参数直接指定，或欢迎界面"打开上次"）
- 左键操作 Button/Switch/Slider（有绑定时自动写回服务器），右键任意组件看详情（绑定值/质量/告警）
- 「连接设置」可覆盖工程内的 host/port/轮询间隔
- 断线自动重连（1s 退避），标签显示 CommLost 灰态

## TCP 数据协议 v1

行文本（UTF-8，`\n` 结尾），可直接 `telnet`/`nc` 调试：

```
PING\n                                  -> PONG\n
READ <n> <idx1> <idx2> ...\n            -> VALUES <n> <idx>:<type>:<value> ...\n
WRITE <idx> <type> <value>\n            -> OK <idx>\n | ERR <message>\n
type ∈ bool|int16|uint16|int32|uint32|float32
无此槽位: VALUES 中该项为 idx:?:-
```

标签 = 槽位索引（0..65535）+ 类型；`工程值 = 原始值 * scale + offset`。
写回：运行器把工程值换算回原始值后 WRITE。

## 帧数据源（TCP/UDP/串口 + 协议配置组件）

「通信」分类下两个画布组件，职责分离、按名称关联：

- **协议配置 ProtocolConfig**：拆帧方式（TLV / 帧头+Length 及参数）+ 规约字段（属性面板「规约字段」区增删编辑：字段名 / TLV 槽位 / 偏移 / 类型 / 长度或枚举；新增字段 TLV 槽位默认与前一条相同（首条为 0）、不自增，按报文实际 T 值填写）；类型含 `u8/i8/u16/i16/u32/i32/f32/f64/`**`bool/string/enum`**（枚举取值为**名称文本**，未命中回退数值；字符串与枚举的字节宽度每字段可配，枚举映射在行内「…」弹窗编辑）；数值字段支持 scale 工程换算（工程值 = 原始值 × scale，默认 1，bool/string/enum 不适用；带 scale 的隐式标签自动升为 Float32）；字段偏移统一相对**负载**（TLV 相对 V；帧头+Length 相对帧头+length 字段之后的数据区）；「AI 配置规约…」弹窗按协议文档/自然语言描述经 LLM（OpenAI 兼容接口，填 Base URL / API Key / 模型名）生成拆帧参数与字段，预览后一键应用到组件；「生成字段组件」按字段类型一键生成显示组件并自动绑定（bool→指示灯，其余类型一律文本，网格排在协议组件下方，同类型且已绑定同字段的跳过；仪表按需手工添加并绑定）；一个协议可被多个数据源复用
- **数据源 DataSource**：传输角色（TCP **客户端**连接远端端口 / TCP **服务端**监听本地端口等待接入 / UDP **服务端**监听本地端口 / UDP **客户端** connect 远端、本地端口由系统临时分配 / **串口** 打开 COM 口，波特率 300..921600、数据位 5..8、校验无/奇/偶、停止位 1/2；串口为字节流，与 TCP 走同一拆帧）+「关联协议」下拉（动态列出工程内全部协议配置组件名）；关联后画布上以蓝色曲线连接两组件（最近边中点）

**显示组件直接绑定协议字段**：文本/仪表/指示灯等组件的属性面板有「绑定协议字段」下拉（候选 = 全部协议字段），选中即存 `bindField`（协议名/字段名）。运行器打开工程时自动合成隐式标签（按字段槽位/类型）与数据绑定（Label→text、Lamp/Switch→isOn、其余→value），无需手工建标签库或关联。画布上绑定协议字段的组件以**青色曲线**连到协议配置组件（与数据源→协议的蓝色曲线区分）。校验面板检查绑定引用（协议/字段不存在）与数据源-协议关联问题。

运行器顶栏「报文监视」实时展示原始帧 HEX 与解析结果。v1 只收不发。配套模拟器 `python tools/udp_frame_sim.py 9001` 与示例工程 `examples/demo_frame_project.json`（数据源 + 泵站TLV协议 + 组件直接绑字段）。

## 单元测试

```bat
scripts\build.cmd x64-debug softg_tests
out\build\x64-debug\softg_tests.exe
```

覆盖：模型 JSON 往返（字节级一致）/ 高版本拒载 / 协议黄金行 / 值文本化 / TCP 回环（含应答分片重组）/ 引擎语义（绑定刷新、联动级联与环截断、告警锁存确认、写回排队、数据质量）。

## 目录结构

```
src/base/       共享库（model 数据模型 / serialize JSON / data TCP 数据层 /
                render 共享渲染器 / runtime 运行引擎 / appshell Win32+DX11 引导 / log）
src/planner/    设计器（PlannerApp / Document(undo) / PlannerContext / panels/*）
src/viewer/     运行器（ViewerApp / PollWorker 后台轮询线程）
tests/          SoftgTest 自研极简单测框架 + 各模块测试
tools/          tcp_sim.py 行协议数据服务器模拟器 / udp_frame_sim.py 帧数据源模拟器（周期发 TLV 帧）
examples/       demo_project.json 演示工程 / demo_frame_project.json 帧数据源演示工程（泵站 TLV）
```

## 关联语义

| 类型 | 含义 | 配置字段 |
|---|---|---|
| dataBinding | 组件属性 ← 标签实时值 | component / property / tag |
| linkage | 源组件事件 → 目标动作（设置属性、页面跳转、显隐切换、写标签、脉冲高亮） | source / event / target / action / param / value |
| alarmRule | 标签越限 → 组件视觉告警（闪烁/描边/变色，可锁存需确认） | tag / comparator / threshold / severity / style / latching / components |

联动传播深度上限 8 层（环防护）；告警默认覆盖"绑定该标签的全部组件"，也可显式指定组件列表。
