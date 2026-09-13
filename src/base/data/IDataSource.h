// 数据源抽象：运行器经此接口轮询/写回标签，与具体协议解耦。
// 阻塞式接口——只在后台 worker 线程调用。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "base/model/Project.h"  // ModbusSettings / Tag

namespace softg {

struct TagReadResult {
    TagName tag;
    bool ok = false;
    TagValue value{};      // 工程值（已乘 scale 加 offset）
    TagQuality quality = TagQuality::Bad;
    std::string error;     // ok=false 时的原因
};

class IDataSource {
public:
    virtual ~IDataSource() = default;

    virtual bool connect(std::string& err) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // 批量读：内部自行分组（<=125 寄存器/请求），返回与输入一一对应的结果
    virtual std::vector<TagReadResult> readTags(const std::vector<const Tag*>& tags) = 0;

    // 写单个标签（value 为工程值，内部换算原始值）
    virtual bool writeTag(const Tag& tag, TagValue value, std::string& err) = 0;

    // 是否支持写回；不支持时 worker 丢弃写请求（不判定连接故障）
    virtual bool supportsWrite() const { return true; }
};

} // namespace softg
