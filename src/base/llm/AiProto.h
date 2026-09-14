// AiProto — LLM 生成的协议配置：响应文本解析 + 应用到协议配置组件。
// 与 UI 解耦（纯逻辑，可单测）；Inspector 的「AI 配置规约」弹窗调用。
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "base/model/Component.h"

namespace pv::llm {

// 提示词（约束 LLM 只输出规定 schema 的 JSON；字段类型覆盖数值/布尔/定长字符串/枚举）
std::string aiProtoSystemPrompt();

struct AiEnumItem {
    int64_t value = 0;
    std::string name;
};

struct AiProtoField {
    std::string name;
    int tagId = 0;          // TLV 槽位
    int offset = 0;         // 相对负载
    std::string type = "u16"; // u8..f64 / bool / string / enum
    bool bigEndian = true;
    int strLen = 16;                  // string 类型字节长度
    std::vector<AiEnumItem> enumItems; // enum 类型映射
};

struct AiProtoResult {
    bool tlv = true;                    // false = 帧头+Length
    // TLV
    int tagBytes = 1, lenBytes = 2;
    bool bigEndian = true, lenIncludesHeader = false;
    // 帧头+Length
    std::string headerHex = "AA 55";
    int lenOffset = 2, lenBytesHeader = 2;
    bool bigEndianHeader = true, lenIncludesAll = false;

    std::vector<AiProtoField> fields;
};

// 解析 LLM 响应文本（容忍 ```json 围栏与前后杂文字）。
// 成功返回 true 且至少解析出 framing；字段为空也允许（err 提示）。
bool parseAiProtoResponse(const std::string& raw, AiProtoResult& out, std::string& err);

// 应用到协议配置组件（写拆帧属性 + fieldCount + f<i>.*；槽位由字段序号自动分配）
void applyAiProto(Component& c, const AiProtoResult& r);

} // namespace pv::llm
