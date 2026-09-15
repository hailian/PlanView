// TestFrameGen — 按拆帧与字段配置生成随机测试帧（编码器，与 FrameCodec 解码对称）。
// 用于验证：生成的 HEX 喂回链路，PageViewer 应能成帧并解析出字段值。
#pragma once

#include <cstdint>
#include <vector>

#include "base/data/frame/FrameSourceSettings.h" // TagField
#include "base/packet/FrameCodec.h"              // FramingConfig

namespace pv {

// 生成 count 组随机帧：
//   TLV 模式：每个字段独立成帧（T|L|V，V 内按字段 offset 前置 0 填充）；
//   帧头+Length：每帧含全部字段（负载按 offset 布局，不足处 0 填充）。
// 随机值：整数取类型全范围、浮点取 [0,100] 两位小数、bool 取 0/1、
// string 填可打印大写字母、enum 从映射表随机取值。seed=0 用随机种子。
std::vector<std::vector<uint8_t>> generateTestFrames(const packet::FramingConfig& fr,
                                                     const std::vector<TagField>& fields,
                                                     int count, uint32_t seed = 0);

// 协议组多帧头：按字段 framingIndex 分组，各组用对应配置生成（配置数须覆盖最大索引）
std::vector<std::vector<uint8_t>> generateTestFrames(
    const std::vector<packet::FramingConfig>& framings, const std::vector<TagField>& fields,
    int count, uint32_t seed = 0);

// 周期自发生成器（数据源「自发送/模拟设备」）：字段值按类型**确定性递增**——
// 无符号整数 0..类型最大值环回、有符号走全量程、浮点 0..100 步进 1 环回、
// 布尔 0/1 翻转、枚举按映射表遍历（空表按宽度环回）、字符串每字节 'a'..'z' 环回、
// hex 每字节 0..255 环回。每周期产出一组帧（TLV=每字段一帧；帧头+Length=一帧含全部字段），
// 与拆帧配置对称、可被接收侧解析回来。
class IncrementalFrameGen {
public:
    IncrementalFrameGen(const packet::FramingConfig& fr, std::vector<TagField> fields);

    // 推进一个周期，返回本周期应发送的帧（字段序即为 TLV 帧序）
    std::vector<std::vector<uint8_t>> nextFrames();

private:
    std::vector<uint8_t> nextFieldBytes(const TagField& f, size_t idx);

    packet::FramingConfig fr_;
    std::vector<TagField> fields_;
    std::vector<uint64_t> counters_; // 每字段周期计数（递增状态）
};

} // namespace pv
