// TestFrameGen — 按拆帧与字段配置生成随机测试帧（编码器，与 FrameCodec 解码对称）。
// 用于验证：生成的 HEX 喂回链路，PageViewer 应能成帧并解析出字段值。
#pragma once

#include <cstdint>
#include <vector>

#include "base/data/frame/FrameSourceSettings.h" // TagField
#include "base/packet/FrameCodec.h"              // FramingConfig

namespace softg {

// 生成 count 组随机帧：
//   TLV 模式：每个字段独立成帧（T|L|V，V 内按字段 offset 前置 0 填充）；
//   帧头+Length：每帧含全部字段（负载按 offset 布局，不足处 0 填充）。
// 随机值：整数取类型全范围、浮点取 [0,100] 两位小数、bool 取 0/1、
// string 填可打印大写字母、enum 从映射表随机取值。seed=0 用随机种子。
std::vector<std::vector<uint8_t>> generateTestFrames(const packet::FramingConfig& fr,
                                                     const std::vector<TagField>& fields,
                                                     int count, uint32_t seed = 0);

} // namespace softg
