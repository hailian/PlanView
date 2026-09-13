// PacketDebug — 报文调试面板：TCP/UDP 可配置接收、16 进制展示、
// TCP 按 TLV 或 帧头+Length 拆帧、自配置规约解析与解析结果展示、HEX 发送。
#pragma once

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "base/packet/DebugConfig.h"
#include "base/packet/FrameCodec.h"
#include "base/packet/TcpLink.h"
#include "base/packet/UdpLink.h"

namespace softg::planner::panels {

using softg::packet::DebugConfig;
using softg::packet::FieldType;
using softg::packet::FrameSplitter;
using softg::packet::PacketField;
using softg::packet::TcpLink;
using softg::packet::UdpLink;

struct PacketFrameEntry {
    uint64_t id = 0;          // 递增编号（选中追踪用）
    bool tx = false;          // true=发送 false=接收
    std::string peer;         // 对端/来源
    std::string timeText;     // HH:MM:SS
    std::vector<uint8_t> data;
};

struct PacketDebugState {
    PacketDebugState() {
        // 默认规约给一组示例字段，配合默认发送报文 "01 F4 42 28 00 00" 即可演示解析
        PacketField f;
        f.name = "温度";
        f.offset = 0;
        f.type = FieldType::U16;
        f.bigEndian = true;
        f.scale = 0.1;
        cfg.spec.fields.push_back(f);
        f.name = "功率";
        f.offset = 2;
        f.type = FieldType::F32;
        f.scale = 1.0;
        cfg.spec.fields.push_back(f);
    }

    DebugConfig cfg;

    UdpLink udp;
    TcpLink tcp;
    FrameSplitter splitter{cfg.framing};

    // 报文日志（收 + 发），上限裁剪
    std::deque<PacketFrameEntry> frames;
    uint64_t nextFrameId = 1;
    uint64_t selectedFrameId = 0;  // 0 = 总是选最新一帧
    static constexpr size_t kMaxFrames = 500;

    // 发送输入
    char sendBuf_[4096] = "01 F4 42 28 00 00";

    // 规约编辑
    char specPath_[512] = "";
    char statusBuf_[256] = "";  // 操作结果/错误提示
    int64_t statusTick = 0;     // 状态消息显示计时

    void pushFrame(bool tx, const std::string& peer, const std::vector<uint8_t>& data);
    void setStatus(const std::string& msg);
};

void drawPacketDebug(PacketDebugState& st);

} // namespace softg::planner::panels
