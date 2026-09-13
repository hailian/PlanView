// DebugConfig — 报文调试接入配置：传输方式 + 拆帧参数 + 解析规约，可存取 JSON。
#pragma once

#include <string>

#include "base/packet/FrameCodec.h"
#include "base/packet/PacketSpec.h"

namespace softg::packet {

enum class Transport : uint8_t { Tcp, Udp };

struct DebugConfig {
    // 传输（默认 UDP 本地 9001 自环，便于开箱演示：监听后直接发送即可收到）
    Transport transport = Transport::Udp;
    std::string host = "127.0.0.1"; // TCP 连接目标 / UDP 发送目标
    int remotePort = 9001;
    int localPort = 9001;           // UDP 本地绑定端口

    // 拆帧（TCP 生效；UDP 数据报天然成帧）
    FramingConfig framing;

    // 解析规约
    PacketSpec spec;
};

namespace debugcfg {

// 保存/加载接入配置（JSON, UTF-8）。失败返回 false + err。
bool save(const std::string& path, const DebugConfig& cfg, std::string& err);
bool load(const std::string& path, DebugConfig& cfg, std::string& err);

} // namespace debugcfg
} // namespace softg::packet
