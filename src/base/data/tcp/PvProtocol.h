// PlanView TCP 数据协议 v1 —— 行文本（UTF-8，\n 结尾），人可读可 telnet 调试。
//
// 请求:
//   PING\n                                   -> PONG\n
//   READ <n> <idx1> <idx2> ...\n             -> VALUES <n> <idx>:<type>:<value> ...\n
//   WRITE <idx> <type> <value>\n             -> OK <idx>\n | ERR <message>\n
// 读取未知槽位: 应答中该项以 idx:?:- 表示（客户端判 Bad）。
// <type> ∈ bool|int16|uint16|int32|uint32|float32；数值一律十进制文本。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/model/Tag.h"

namespace pv::tcp {

// ---- 请求构造 ----
std::string encodePing();
std::string encodeReadReq(const std::vector<int>& indices);
std::string encodeWriteReq(int index, TagDataType type, const std::string& valueText);

// ---- 应答解析 ----
struct ReadItem {
    int index = -1;
    TagDataType type = TagDataType::UInt16;
    TagValue value{};   // 解析后的值（bool/int64/double）
    bool ok = false;    // false = 服务器无此槽位（"?"/"-"）
};

// 解析 "VALUES n idx:type:value ..."；整行非法返回 false
bool parseValuesLine(std::string_view line, std::vector<ReadItem>& out);

// 解析 "OK <idx>" / "ERR <msg>"
struct WriteAck { bool ok; int index; std::string error; };
WriteAck parseWriteAck(std::string_view line);

// 值 <-> 文本
std::string tagValueToText(const TagValue& v);
std::optional<TagValue> parseTypedValue(std::string_view type, std::string_view text);

} // namespace pv::tcp
