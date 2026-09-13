// HexUtil — 字节串 <-> 16 进制文本。报文展示与手写报文输入共用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace softg::packet {

// "AA BB CC"（大写、单空格分隔）
std::string bytesToHex(const std::vector<uint8_t>& bytes);

// 容错解析：分隔符可为空格/逗号/换行/冒号，也可无分隔（"AABBCC"）。
// 非法字符或奇数个 hex 位时返回 false，err 给出原因。
bool hexToBytes(const std::string& text, std::vector<uint8_t>& out, std::string& err);

} // namespace softg::packet
