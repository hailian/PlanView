# AGENTS.md — SoftG 协作指南

面向 AI 编码代理与本仓库协作者的速查。详细功能介绍见 [README.md](README.md)。

## 项目概况

SoftG — Windows 专用 HMI 组态软件套件（C++20 / Dear ImGui v1.92.9b-docking / Win32 + DX11 / WinSock2），一个 CMake 工程产出三个目标：

| 目标 | 类型 | 说明 |
|---|---|---|
| `softg_base` | 静态库 | 两款 exe 共用：model / serialize / data(TCP) / render / runtime / appshell / log |
| `LogicPlanner` | exe | 设计器：拖拽组态、编辑关联、生成 `.json` 工程 |
| `PageViewer` | exe | 运行器：加载工程、TCP 轮询刷新、交互写回 |

**核心架构不变量**：组件绘制代码只存在于 `src/base/render/ComponentRenderer`，设计器画布与运行器渲染调用同一函数（"所见即所得"由链接期保证）。改组件外观只改这一处，绝不在 exe 侧重写绘制。

## 构建与测试

依赖（imgui / nlohmann/json）经 CMake FetchContent 拉取，**首次配置需联网**（默认 gitee 镜像，可 `-D SOFTG_IMGUI_REPO=...` 覆盖）。需要 VS 2022（含 C++ 桌面开发与 CMake 组件），`scripts/build.cmd` 会自动调 vcvars64。

```bat
:: cmd / VS 开发者环境
scripts\build.cmd x64-debug                :: 配置 + 全量构建（preset 另有 x64-release、x86-*）
scripts\build.cmd x64-debug softg_tests    :: 第二参数可指定目标（如只编测试）
scripts\build.cmd x64-debug PageViewer
```

```bash
# Git Bash 中调 cmd 脚本
cmd //c "scripts\\build.cmd x64-debug"
```

产物在 `out/build/<preset>/src/planner/LogicPlanner.exe`、`out/build/<preset>/src/viewer/PageViewer.exe`。

### 单元测试

自研零依赖框架 SoftgTest（`tests/SoftgTest.h`），宏用法近似 doctest：`TEST_CASE("名") { CHECK(...); REQUIRE(...); }`。所有测试链成单个 `softg_tests.exe`：

```bat
scripts\build.cmd x64-debug softg_tests
out\build\x64-debug\tests\softg_tests.exe   :: 或 ctest --preset x64-debug
```

**新增测试文件**：在 `tests/` 下建 `.cpp`，直接 `#include "SoftgTest.h"` 即可（**不要**定义 `SOFTG_TEST_MAIN`，main 已在 `test_Smoke.cpp` 中），然后把文件加进 `tests/CMakeLists.txt`。

**提交/交付前必须**：全量构建通过 + `softg_tests` 全部通过。

### 手工端到端验证（UI / 协议改动）

```bat
python tools\tcp_sim.py 9000                                        :: 数据服务器模拟器
out\build\x64-debug\src\viewer\PageViewer.exe examples\demo_project.json
```

预期：仪表/曲线随模拟数据刷新；按钮写回槽位 7；阈值告警闪烁/描边；页面跳转；滑块写回槽位 6。GUI 行为无法自动化时，以此演示工程手动核对。

## 目录结构

```
src/base/          共享静态库
  model/           数据模型（纯值类型）+ ComponentRegistry 内置组件类型注册表
  serialize/       JSON 工程读写（ProjectJson + JsonHelpers）
  data/            数据源抽象、SoftgProtocol 行文本协议、TcpDataSource
  render/          共享组件渲染器（唯一绘制入口）+ TextureCache + widgets
  runtime/         RuntimeEngine：绑定刷新 / 联动级联 / 告警 / 写回排队
  appshell/        Win32+DX11 窗口引导、文件对话框、中文字体
  log/             日志
src/planner/       设计器（Document 承担 undo/redo；panels/ 下各 UI 面板）
src/viewer/        运行器（ViewerApp；PollWorker 后台轮询线程）
tests/             SoftgTest 框架 + 各模块测试（一个 exe）
tools/tcp_sim.py   TCP 数据服务器模拟器（协议参考实现）
examples/          demo_project.json 演示工程
```

## 领域不变量（改相关代码前必读）

- **JSON 工程文件**：序列化必须保证**字节级往返一致**（test_ModelJson 覆盖）；`version` 字段只升不降，加载时**高版本拒载**。改模型结构必须同步 `serialize/` 并补/改往返测试。
- **标签（Tag）** = 槽位索引 0..65535 + 数据类型；`工程值 = 原始值 * scale + offset`，写反向换算后 WRITE。
- **三类关联**：`dataBinding`（组件属性←标签）、`linkage`（事件→动作）、`alarmRule`（越限→视觉告警，可锁存需确认）。联动传播深度上限 **8 层**（环防护），在 RuntimeEngine 中实现。
- **TCP 协议 v1**（行文本 UTF-8，`\n` 结尾；PING/READ/WRITE）是线上契约：改动必须同步 `SoftgProtocol`、`TcpDataSource`、`tools/tcp_sim.py` 与 test_TcpData 的协议黄金行。
- **新增组件类型**：在 `ComponentRegistry` 构造中注册 `ComponentTypeInfo`（含 PropertySpec 默认属性），在 `ComponentRenderer` 中实现绘制；类型按 category 分组（显示/操作/容器/图形）。

## 代码风格

- 源码含**中文注释与中文字符串**：文件必须保存为 **UTF-8**（MSVC `/utf-8` 已全局开启，存成 GBK 会编译错误或乱码）。
- 命名空间 `softg`；类型 PascalCase，函数 camelCase，类私有成员尾下划线（`types_`）；模型结构体为公开成员的纯值类型（拷贝即快照，undo 依赖值语义）。
- 头文件用 `#pragma once`；`base` 内 include 路径写 `base/model/Component.h` 形式（include root 是 `src/`）。
- 注释风格与现有代码一致：中文，写"为什么/约束"，关键协议与语义处保留说明。

## Git 约定

- 提交信息：中文一句话概述（沿用现有历史风格，如「重构项目为模块化多目标架构」）。
- 不要提交构建产物与运行时文件（`.gitignore` 已覆盖 `out/`、`*.obj`、`*.pdb`、`*.ini`、`PageViewer.recent` 等）；根目录若出现 `fcheck.obj`、`vc140.pdb` 之类编译遗留物属正常，忽略即可。
