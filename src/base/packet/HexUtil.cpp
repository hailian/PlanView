#include "base/packet/HexUtil.h"

#include <cctype>
#include <cstdio>

namespace pv::packet {

std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) out += ' ';
        out += kHex[bytes[i] >> 4];
        out += kHex[bytes[i] & 0x0F];
    }
    return out;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexToBytes(const std::string& text, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    std::vector<int> nibbles;
    nibbles.reserve(text.size());
    for (char c : text) {
        if (std::isspace((unsigned char)c) || c == ',' || c == ':' || c == '-' || c == ';')
            continue; // 常见分隔符
        int v = hexVal(c);
        if (v < 0) {
            err = "非法字符: '";
            err += c;
            err += "'";
            return false;
        }
        nibbles.push_back(v);
    }
    if (nibbles.size() % 2 != 0) {
        err = "16 进制位数为奇数";
        return false;
    }
    out.reserve(nibbles.size() / 2);
    for (size_t i = 0; i < nibbles.size(); i += 2)
        out.push_back((uint8_t)((nibbles[i] << 4) | nibbles[i + 1]));
    return true;
}

} // namespace pv::packet
