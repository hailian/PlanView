#include "base/model/Tag.h"

namespace softg {

const char* tagDataTypeName(TagDataType t) {
    switch (t) {
    case TagDataType::Bool: return "bool";
    case TagDataType::Int16: return "int16";
    case TagDataType::UInt16: return "uint16";
    case TagDataType::Int32: return "int32";
    case TagDataType::UInt32: return "uint32";
    case TagDataType::Float32: return "float32";
    case TagDataType::String: return "string";
    }
    return "?";
}

std::optional<TagDataType> tagDataTypeFromName(std::string_view name) {
    for (uint8_t i = 0; i <= (uint8_t)TagDataType::String; ++i) {
        TagDataType t = (TagDataType)i;
        if (name == tagDataTypeName(t)) return t;
    }
    return std::nullopt;
}

double Tag::numeric() const {
    if (auto p = std::get_if<bool>(&currentValue)) return *p ? 1.0 : 0.0;
    if (auto p = std::get_if<int64_t>(&currentValue)) return (double)*p;
    if (auto p = std::get_if<double>(&currentValue)) return *p;
    return 0.0;
}

} // namespace softg
