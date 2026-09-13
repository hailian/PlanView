#include "base/packet/PacketSpec.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace softg::packet {

const char* fieldTypeToString(FieldType t) {
    switch (t) {
    case FieldType::U8: return "u8";
    case FieldType::I8: return "i8";
    case FieldType::U16: return "u16";
    case FieldType::I16: return "i16";
    case FieldType::U32: return "u32";
    case FieldType::I32: return "i32";
    case FieldType::F32: return "f32";
    case FieldType::F64: return "f64";
    case FieldType::Bool: return "bool";
    case FieldType::String: return "string";
    case FieldType::Enum: return "enum";
    case FieldType::Hex: return "hex";
    case FieldType::Ascii: return "ascii";
    }
    return "u16";
}

bool fieldTypeFromString(const std::string& s, FieldType& out) {
    static const struct { const char* name; FieldType t; } kMap[] = {
        {"u8", FieldType::U8},   {"i8", FieldType::I8},
        {"u16", FieldType::U16}, {"i16", FieldType::I16},
        {"u32", FieldType::U32}, {"i32", FieldType::I32},
        {"f32", FieldType::F32}, {"f64", FieldType::F64},
        {"bool", FieldType::Bool}, {"string", FieldType::String},
        {"enum", FieldType::Enum},
        {"hex", FieldType::Hex}, {"ascii", FieldType::Ascii},
    };
    for (const auto& e : kMap)
        if (s == e.name) { out = e.t; return true; }
    return false;
}

int fieldTypeBytes(FieldType t) {
    switch (t) {
    case FieldType::U8: case FieldType::I8: return 1;
    case FieldType::U16: case FieldType::I16: return 2;
    case FieldType::U32: case FieldType::F32: return 4;
    case FieldType::F64: return 8;
    case FieldType::Bool: return 1;
    case FieldType::String: case FieldType::Enum: return 0; // 长度/宽度可配
    case FieldType::Hex: case FieldType::Ascii: return 0;   // 长度可配置
    }
    return 0;
}

namespace {

uint64_t readUintBE(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

uint64_t readUintLE(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// 补码符号扩展到 int64
int64_t signExtend(uint64_t v, int bytes) {
    int bits = bytes * 8;
    if (bytes < 8 && (v & (1ULL << (bits - 1))))
        v |= ~0ULL << bits;
    return (int64_t)v;
}

void formatDouble(double v, char* buf, size_t n) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(buf, n, "%.0f", v);
    else
        std::snprintf(buf, n, "%.6g", v);
}

} // namespace

std::vector<ParsedField> parsePacket(const std::vector<PacketField>& fields,
                                     const std::vector<uint8_t>& payload) {
    std::vector<ParsedField> out;
    out.reserve(fields.size());
    for (const auto& f : fields) {
        ParsedField r;
        r.name = f.name;
        r.offset = f.offset;
        r.length = f.length;
        r.engText = r.rawText = r.rawHex = "";

        bool varLen = f.type == FieldType::Hex || f.type == FieldType::Ascii ||
                      f.type == FieldType::String || f.type == FieldType::Enum;
        int bytes = varLen ? f.length : fieldTypeBytes(f.type);
        if (f.offset < 0 || f.length <= 0 || bytes <= 0 ||
            (int64_t)f.offset + bytes > (int64_t)payload.size()) {
            r.ok = false;
            r.rawText = "(越界)";
            out.push_back(std::move(r));
            continue;
        }
        const uint8_t* p = payload.data() + f.offset;
        r.rawHex.clear();
        char hex[4];
        for (int i = 0; i < bytes; ++i) {
            std::snprintf(hex, sizeof(hex), "%02X", p[i]);
            r.rawHex += (i ? " " : "");
            r.rawHex += hex;
        }
        r.ok = true;

        char buf[64];
        switch (f.type) {
        case FieldType::U8: case FieldType::U16: case FieldType::U32: {
            uint64_t v = f.bigEndian ? readUintBE(p, bytes) : readUintLE(p, bytes);
            std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
            r.rawText = buf;
            formatDouble((double)v * f.scale + f.offsetValue, buf, sizeof(buf));
            r.engText = buf;
            break;
        }
        case FieldType::I8: case FieldType::I16: case FieldType::I32: {
            uint64_t u = f.bigEndian ? readUintBE(p, bytes) : readUintLE(p, bytes);
            int64_t v = signExtend(u, bytes);
            std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
            r.rawText = buf;
            formatDouble((double)v * f.scale + f.offsetValue, buf, sizeof(buf));
            r.engText = buf;
            break;
        }
        case FieldType::F32: {
            static_assert(sizeof(float) == 4);
            uint32_t u = (uint32_t)(f.bigEndian ? readUintBE(p, 4) : readUintLE(p, 4));
            float v;
            std::memcpy(&v, &u, 4);
            std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
            r.rawText = buf;
            formatDouble((double)v * f.scale + f.offsetValue, buf, sizeof(buf));
            r.engText = buf;
            break;
        }
        case FieldType::F64: {
            static_assert(sizeof(double) == 8);
            uint64_t u = f.bigEndian ? readUintBE(p, 8) : readUintLE(p, 8);
            double v;
            std::memcpy(&v, &u, 8);
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            r.rawText = buf;
            formatDouble(v * f.scale + f.offsetValue, buf, sizeof(buf));
            r.engText = buf;
            break;
        }
        case FieldType::Bool: {
            bool v = p[0] != 0;
            r.rawText = v ? "true" : "false";
            r.engText = r.rawText;
            break;
        }
        case FieldType::String: {
            int n = bytes;  // 去尾部 0x00/0xFF 填充，保留原始字节（UTF-8 友好）
            while (n > 0 && (p[n - 1] == 0x00 || p[n - 1] == 0xFF)) --n;
            r.rawText.assign((const char*)p, (size_t)n);
            r.engText = r.rawText;
            break;
        }
        case FieldType::Enum: {
            uint64_t v = f.bigEndian ? readUintBE(p, bytes) : readUintLE(p, bytes);
            if (const std::string* nm = findEnumName(f.enums, (int64_t)v)) {
                r.rawText = *nm;
            } else {
                std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
                r.rawText = buf;
            }
            r.engText = r.rawText;
            break;
        }
        case FieldType::Hex: {
            r.rawText = r.rawHex;
            r.engText = r.rawHex;
            break;
        }
        case FieldType::Ascii: {
            std::string s;
            for (int i = 0; i < bytes; ++i)
                s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
            r.rawText = s;
            r.engText = s;
            break;
        }
        }
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace softg::packet
