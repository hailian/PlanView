#include "planner/panels/PacketDebug.h"

#include <algorithm>
#include <ctime>

#include "base/appshell/FileDialog.h"
#include "base/packet/HexUtil.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"  // InputText(std::string*) 重载

namespace softg::planner::panels {

using namespace softg::packet;

namespace {

std::string nowTimeText() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

const char* kTransports[] = {"UDP", "TCP 客户端"};
const char* kFrameModes[] = {"TLV (T|L|V)", "固定帧头 + Length"};
const char* kFieldTypes[] = {"u8", "i8", "u16", "i16", "u32", "i32", "f32", "f64", "hex", "ascii"};

// 日志行 HEX 预览最长字节数，超出省略
constexpr size_t kPreviewBytes = 12;

} // namespace

void PacketDebugState::pushFrame(bool tx, const std::string& peer,
                                 const std::vector<uint8_t>& data) {
    PacketFrameEntry e;
    e.id = nextFrameId++;
    e.tx = tx;
    e.peer = peer;
    e.timeText = nowTimeText();
    e.data = data;
    frames.push_back(std::move(e));
    while (frames.size() > kMaxFrames)
        frames.pop_front();
}

void PacketDebugState::setStatus(const std::string& msg) {
    std::snprintf(statusBuf_, sizeof(statusBuf_), "%s", msg.c_str());
    statusTick = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
}

void drawPacketDebug(PacketDebugState& st) {
    if (!ImGui::Begin("报文调试"))
        return ImGui::End();

    // 拆帧配置同步（模式/帧头变化时内部缓冲自动重置）
    st.splitter.setConfig(st.cfg.framing);
    // UDP 发送目标同步（面板编辑的是配置值，链路发送前需感知）
    st.udp.setRemote(st.cfg.host, st.cfg.remotePort);

    // ---- 收包：UDP 数据报直接成帧；TCP 字节流喂拆帧器 ----
    std::deque<UdpPacket> udpIn;
    st.udp.drain(udpIn);
    while (!udpIn.empty()) {
        st.pushFrame(false, udpIn.front().from, udpIn.front().data);
        udpIn.pop_front();
    }
    std::deque<TcpChunk> tcpIn;
    st.tcp.drain(tcpIn);
    std::vector<std::vector<uint8_t>> newFrames;
    while (!tcpIn.empty()) {
        st.splitter.feed(tcpIn.front().data.data(), tcpIn.front().data.size(), newFrames);
        tcpIn.pop_front();
    }
    for (auto& f : newFrames)
        st.pushFrame(false, st.cfg.host + ":" + std::to_string(st.cfg.remotePort), f);

    // ================= 传输与接收 =================
    if (ImGui::CollapsingHeader("传输", ImGuiTreeNodeFlags_DefaultOpen)) {
        int tr = st.cfg.transport == Transport::Udp ? 0 : 1;
        if (ImGui::Combo("方式", &tr, kTransports, IM_ARRAYSIZE(kTransports)))
            st.cfg.transport = tr == 0 ? Transport::Udp : Transport::Tcp;
        float w = ImGui::GetFontSize() * 5.5f;

        if (st.cfg.transport == Transport::Udp) {
            ImGui::SetNextItemWidth(w * 1.5f);
            ImGui::InputInt("本地端口", &st.cfg.localPort, 0, 0); // 无步进按钮，留足显示宽度
            st.cfg.localPort = std::clamp(st.cfg.localPort, 1, 65535);
            ImGui::SameLine();
            if (st.udp.isRunning()) {
                if (ImGui::Button("停止监听")) st.udp.stop();
            } else {
                if (ImGui::Button("启动监听")) {
                    std::string err;
                    if (!st.udp.start(st.cfg.localPort, err)) st.setStatus(err);
                }
            }
            ImGui::SetNextItemWidth(w);
            ImGui::InputText("目标IP", &st.cfg.host);
            ImGui::SetNextItemWidth(w * 1.5f);
            ImGui::InputInt("目标端口", &st.cfg.remotePort, 0, 0);
            st.cfg.remotePort = std::clamp(st.cfg.remotePort, 1, 65535);
            ImGui::SameLine();
            ImGui::TextUnformatted(st.udp.isRunning() ? "(监听中)" : "(未监听)");
        } else {
            ImGui::SetNextItemWidth(w * 1.6f);
            ImGui::InputText("主机", &st.cfg.host);
            ImGui::SetNextItemWidth(w);
            ImGui::InputInt("端口", &st.cfg.remotePort);
            st.cfg.remotePort = std::clamp(st.cfg.remotePort, 1, 65535);
            ImGui::SameLine();
            if (st.tcp.isConnected()) {
                if (ImGui::Button("断开")) st.tcp.disconnect();
            } else {
                if (ImGui::Button("连接")) {
                    std::string err;
                    if (!st.tcp.connect(st.cfg.host, st.cfg.remotePort, err))
                        st.setStatus(err);
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(st.tcp.isConnected() ? "已连接" : "未连接");
            std::string terr = st.tcp.lastError();
            if (!terr.empty() && !st.tcp.isConnected())
                ImGui::TextDisabled("%s", terr.c_str());
        }
    }

    // ================= 拆帧设置（TCP 字节流才需要） =================
    if (st.cfg.transport == Transport::Tcp &&
        ImGui::CollapsingHeader("TCP 拆帧", ImGuiTreeNodeFlags_DefaultOpen)) {
        int fm = st.cfg.framing.mode == FrameMode::Tlv ? 0 : 1;
        if (ImGui::Combo("拆解模式", &fm, kFrameModes, IM_ARRAYSIZE(kFrameModes)))
            st.cfg.framing.mode = fm == 0 ? FrameMode::Tlv : FrameMode::HeaderLength;
        float w = ImGui::GetFontSize() * 4.0f;
        if (st.cfg.framing.mode == FrameMode::Tlv) {
            ImGui::SetNextItemWidth(w);
            ImGui::InputInt("T 字节数", &st.cfg.framing.tagBytes);
            ImGui::SetNextItemWidth(w);
            ImGui::InputInt("L 字节数", &st.cfg.framing.lenBytes);
            ImGui::Checkbox("大端", &st.cfg.framing.bigEndian);
            ImGui::SameLine();
            ImGui::Checkbox("L 含帧头", &st.cfg.framing.lenIncludesHeader);
        } else {
            std::string hex = bytesToHex(st.cfg.framing.header);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", hex.c_str());
            ImGui::SetNextItemWidth(w * 2.2f);
            if (ImGui::InputText("帧头HEX", buf, sizeof(buf))) {
                std::vector<uint8_t> bytes;
                std::string err;
                if (hexToBytes(buf, bytes, err)) {
                    if (!bytes.empty()) st.cfg.framing.header = bytes;
                } else {
                    st.setStatus("帧头: " + err);
                }
            }
            ImGui::SetNextItemWidth(w);
            ImGui::InputInt("length 偏移", &st.cfg.framing.lenOffset);
            ImGui::SetNextItemWidth(w);
            ImGui::InputInt("length 字节数", &st.cfg.framing.lenBytesHeader);
            ImGui::Checkbox("大端", &st.cfg.framing.bigEndianHeader);
            ImGui::SameLine();
            ImGui::Checkbox("length 为整帧长", &st.cfg.framing.lenIncludesAll);
        }
        st.cfg.framing.tagBytes = std::clamp(st.cfg.framing.tagBytes, 1, 4);
        st.cfg.framing.lenBytes = std::clamp(st.cfg.framing.lenBytes, 1, 4);
        st.cfg.framing.lenBytesHeader = std::clamp(st.cfg.framing.lenBytesHeader, 1, 4);
        st.cfg.framing.lenOffset = std::max(st.cfg.framing.lenOffset, 0);
        ImGui::SetNextItemWidth(w);
        ImGui::InputInt("帧长上限", &st.cfg.framing.maxFrameLen);
        st.cfg.framing.maxFrameLen = std::clamp(st.cfg.framing.maxFrameLen, 8, 65535);
    }

    // ================= 解析规约 =================
    if (ImGui::CollapsingHeader("解析规约", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("添加字段")) {
            PacketField f;
            f.name = "字段" + std::to_string(st.cfg.spec.fields.size() + 1);
            f.offset = st.cfg.spec.fields.empty()
                           ? 0
                           : st.cfg.spec.fields.back().offset + st.cfg.spec.fields.back().length;
            st.cfg.spec.fields.push_back(std::move(f));
        }
        ImGui::SameLine();
        if (ImGui::Button("删除末尾")) {
            if (!st.cfg.spec.fields.empty()) st.cfg.spec.fields.pop_back();
        }
        ImGui::SameLine();
        if (ImGui::Button("保存配置")) {
            std::string path;
            std::string def = st.cfg.spec.name + ".json";
            std::string err;
            if (dialog::saveFile("保存报文接入配置", {{"报文配置 (*.json)", "*.json"}}, def, path))
                st.setStatus(debugcfg::save(path, st.cfg, err) ? "已保存: " + path
                                                               : "保存失败: " + err);
        }
        ImGui::SameLine();
        if (ImGui::Button("加载配置")) {
            std::string path;
            if (dialog::openFile("加载报文接入配置", {{"报文配置 (*.json)", "*.json"}}, path)) {
                DebugConfig loaded;
                std::string err;
                if (debugcfg::load(path, loaded, err)) {
                    st.cfg = std::move(loaded);
                    st.setStatus("已加载: " + path);
                } else {
                    st.setStatus("加载失败: " + err);
                }
            }
        }

        if (!st.cfg.spec.fields.empty() && ImGui::BeginTable("spec", 7)) {
            ImGui::TableSetupColumn("字段名");
            ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("字节数", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("大端", ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("比例", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("偏移量", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < st.cfg.spec.fields.size(); ++i) {
                PacketField& f = st.cfg.spec.fields[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##n", &f.name);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputInt("##o", &f.offset, 0, 0);
                ImGui::TableNextColumn();
                int bytes = fieldTypeBytes(f.type);
                if (bytes > 0) {
                    f.length = bytes;
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Text("%d", f.length);
                } else {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputInt("##l", &f.length, 0, 0);
                    f.length = std::clamp(f.length, 1, 128);
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                int t = (int)f.type;
                if (ImGui::Combo("##t", &t, kFieldTypes, IM_ARRAYSIZE(kFieldTypes))) {
                    f.type = (FieldType)t;
                    if (int n = fieldTypeBytes(f.type)) f.length = n;
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::Checkbox("##e", &f.bigEndian);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##s", &f.scale, 0.0, 0.0, "%.4g");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##v", &f.offsetValue, 0.0, 0.0, "%.4g");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ================= 发送 =================
    if (ImGui::CollapsingHeader("发送 HEX", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputTextMultiline("##send", st.sendBuf_, sizeof(st.sendBuf_),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3));
        if (ImGui::Button("发送")) {
            std::vector<uint8_t> bytes;
            std::string err;
            if (!hexToBytes(st.sendBuf_, bytes, err)) {
                st.setStatus("HEX 解析失败: " + err);
            } else if (bytes.empty()) {
                st.setStatus("内容为空");
            } else if (st.cfg.transport == Transport::Udp) {
                if (st.udp.send(bytes, err)) {
                    st.pushFrame(true, st.cfg.host + ":" + std::to_string(st.cfg.remotePort),
                                 bytes);
                    st.setStatus("已发送 " + std::to_string(bytes.size()) + " 字节 (UDP)");
                } else {
                    st.setStatus(err);
                }
            } else {
                if (st.tcp.send(bytes, err)) {
                    st.pushFrame(true, st.cfg.host + ":" + std::to_string(st.cfg.remotePort),
                                 bytes);
                    st.setStatus("已发送 " + std::to_string(bytes.size()) + " 字节 (TCP)");
                } else {
                    st.setStatus(err);
                }
            }
        }
        ImGui::SameLine();
        if (*st.statusBuf_)
            ImGui::TextDisabled("%s", st.statusBuf_);
    }

    // ================= 报文列表 =================
    if (ImGui::CollapsingHeader("报文列表", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted("收到的报文按传输层成帧展示；点击行查看 HEX 与规约解析");
        ImGui::BeginChild("frames", ImVec2(0, ImGui::GetTextLineHeight() * 10),
                          ImGuiChildFlags_Borders);
        if (st.frames.empty())
            ImGui::TextDisabled("(暂无报文)");
        for (size_t i = 0; i < st.frames.size(); ++i) {
            const PacketFrameEntry& e = st.frames[i];
            ImGui::PushID((int)e.id);
            std::string preview;
            for (size_t k = 0; k < e.data.size() && k < kPreviewBytes; ++k) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02X", e.data[k]);
                preview += (k ? " " : "") + std::string(b);
            }
            if (e.data.size() > kPreviewBytes) preview += " ...";
            char line[512];
            std::snprintf(line, sizeof(line), "%s %s [%s] %zu 字节  %s", e.tx ? "TX" : "RX",
                          e.timeText.c_str(), e.peer.c_str(), e.data.size(), preview.c_str());
            bool selected = e.id == st.selectedFrameId ||
                            (st.selectedFrameId == 0 && i + 1 == st.frames.size());
            if (ImGui::Selectable(line, selected)) st.selectedFrameId = e.id;
            ImGui::PopID();
        }
        // 自动滚动到底部
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // ================= 帧详情 + 规约解析 =================
    if (ImGui::CollapsingHeader("帧详情与规约解析", ImGuiTreeNodeFlags_DefaultOpen)) {
        const PacketFrameEntry* sel = nullptr;
        if (!st.frames.empty()) {
            if (st.selectedFrameId != 0) {
                for (const auto& e : st.frames)
                    if (e.id == st.selectedFrameId) sel = &e;
            }
            if (!sel) sel = &st.frames.back();
        }
        if (!sel) {
            ImGui::TextDisabled("(无选中报文)");
        } else {
            ImGui::Text("%s %s [%s]  %zu 字节", sel->tx ? "TX" : "RX", sel->timeText.c_str(),
                        sel->peer.c_str(), sel->data.size());
            // HEX DUMP：每行 16 字节
            ImGui::BeginChild("dump", ImVec2(0, ImGui::GetTextLineHeight() * 6),
                              ImGuiChildFlags_Borders);
            for (size_t base = 0; base < sel->data.size(); base += 16) {
                char line[256];
                int n = std::snprintf(line, sizeof(line), "%04zX  ", base);
                for (size_t k = 0; k < 16; ++k) {
                    if (base + k < sel->data.size())
                        n += std::snprintf(line + n, sizeof(line) - n, "%02X ",
                                           sel->data[base + k]);
                    else
                        n += std::snprintf(line + n, sizeof(line) - n, "   ");
                    if (k == 7) n += std::snprintf(line + n, sizeof(line) - n, " ");
                }
                n += std::snprintf(line + n, sizeof(line) - n, " |");
                for (size_t k = 0; k < 16 && base + k < sel->data.size(); ++k) {
                    uint8_t c = sel->data[base + k];
                    n += std::snprintf(line + n, sizeof(line) - n, "%c",
                                       (c >= 0x20 && c < 0x7F) ? c : '.');
                }
                ImGui::TextUnformatted(line);
            }
            ImGui::EndChild();

            // 规约解析结果
            auto parsed = parsePacket(st.cfg.spec.fields, sel->data);
            if (ImGui::BeginTable("parsed", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("字段");
                ImGui::TableSetupColumn("偏移");
                ImGui::TableSetupColumn("原始HEX");
                ImGui::TableSetupColumn("原始值");
                ImGui::TableSetupColumn("工程值");
                ImGui::TableHeadersRow();
                for (const auto& p : parsed) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", p.offset);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.rawHex.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.rawText.c_str());
                    ImGui::TableNextColumn();
                    if (p.ok)
                        ImGui::TextUnformatted(p.engText.c_str());
                    else
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", p.rawText.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("解析按当前规约对整帧执行：偏移相对帧首（TLV 含 T|L）");
        }
    }

    ImGui::End();
}

} // namespace softg::planner::panels
