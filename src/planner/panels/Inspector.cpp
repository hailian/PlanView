#include "planner/panels/Inspector.h"

#include "base/data/frame/FrameSourceSettings.h"
#include "base/llm/AiProto.h"
#include "base/llm/LlmClient.h"
#include "base/model/ComponentRegistry.h"
#include "base/packet/NpcapApi.h"
#include "base/packet/PacketSpec.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "planner/PlannerContext.h"

#include <atomic>
#include <thread>

namespace pv::planner::panels {

namespace {

// 规约字段类型候选（与 packet::FieldType 的可映射子集）
const char* kFieldTypes[] = {"u8",  "i8",   "u16",  "i16", "u32", "i32",
                             "f32", "f64",  "bool", "string", "enum"};
const int kFieldTypeCount = 11;

// 枚举字段编辑弹窗：字节宽度(1/2/4) + 值→名称行（存 f<i>.len / enumCount / e<j>.v|n）
void drawEnumEditor(Component& c, PlannerContext& ctx, const std::string& p) {
    int64_t w = props::asInt(c.propOr(p + "len", int64_t(1)));
    int width = (w == 2 || w == 4) ? (int)w : 1;
    ImGui::TextUnformatted("字节宽度");
    const int widths[3] = {1, 2, 4};
    for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine();
        bool sel = width == widths[k];
        std::string lbl = std::to_string(widths[k]) + "B";
        if (ImGui::RadioButton(lbl.c_str(), sel) && !sel) {
            ctx.doc.commit("枚举宽度");
            c.setProp(p + "len", int64_t(widths[k]));
        }
    }
    ImGui::Separator();
    int64_t count64 = props::asInt(c.propOr(p + "enumCount", int64_t(0)));
    int count = (int)std::clamp<int64_t>(count64, 0, 64);
    if (ImGui::Button("添加项")) {
        ctx.doc.commit("添加枚举项");
        std::string ep = p + "e" + std::to_string(count) + ".";
        c.setProp(ep + "v", int64_t(count));
        c.setProp(ep + "n", std::string("项" + std::to_string(count + 1)));
        c.setProp(p + "enumCount", int64_t(count + 1));
        ++count;
    }
    ImGui::SameLine();
    if (count > 0 && ImGui::Button("删除末尾")) {
        ctx.doc.commit("删除枚举项");
        c.setProp(p + "enumCount", int64_t(count - 1));
        --count;
    }
    if (count > 0 &&
        ImGui::BeginTable("enums", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("值", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        for (int j = 0; j < count; ++j) {
            std::string ep = p + "e" + std::to_string(j) + ".";
            ImGui::PushID(j);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int64_t v = props::asInt(c.propOr(ep + "v", int64_t(0)));
            int iv = (int)v;
            if (ImGui::InputInt("##v", &iv, 0, 0)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("枚举值");
                c.setProp(ep + "v", int64_t(iv));
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            std::string nm = props::asString(c.propOr(ep + "n", std::string()));
            if (ImGui::InputText("##n", &nm)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("枚举名称");
                c.setProp(ep + "n", nm);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// 「AI 配置规约」弹窗状态（请求跨帧；模态弹窗保证选中组件不变）
struct AiDialogState {
    char input[4096] = "";
    llm::LlmConfig cfg;
    char baseUrlBuf[512] = "";
    char apiKeyBuf[256] = "";
    char modelBuf[64] = "";
    bool cfgLoaded = false;
    std::thread worker;
    std::atomic<int> phase{0}; // 0=空闲 1=请求中 2=成功 3=失败
    std::string response;
    std::string error;
    llm::AiProtoResult result;

    ~AiDialogState() {
        if (worker.joinable()) worker.join();
    }
};
AiDialogState& aiDialog() {
    static AiDialogState s;
    return s;
}

void drawAiProtoDialog(Component& c, PlannerContext& ctx) {
    AiDialogState& st = aiDialog();
    if (!ImGui::BeginPopupModal("AI 配置规约", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // ---- LLM 设置（可折叠；首次打开加载本地配置） ----
    if (!st.cfgLoaded) {
        st.cfgLoaded = true;
        llm::loadConfig(st.cfg);
        snprintf(st.baseUrlBuf, sizeof(st.baseUrlBuf), "%s", st.cfg.baseUrl.c_str());
        snprintf(st.apiKeyBuf, sizeof(st.apiKeyBuf), "%s", st.cfg.apiKey.c_str());
        snprintf(st.modelBuf, sizeof(st.modelBuf), "%s", st.cfg.model.c_str());
    }
    if (ImGui::CollapsingHeader("LLM 设置")) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##url", st.baseUrlBuf, sizeof(st.baseUrlBuf));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##key", st.apiKeyBuf, sizeof(st.apiKeyBuf),
                         ImGuiInputTextFlags_Password);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##model", st.modelBuf, sizeof(st.modelBuf));
        ImGui::TextDisabled("Base URL / API Key / 模型名（OpenAI 兼容接口）");
        if (ImGui::Button("保存配置")) {
            st.cfg.baseUrl = st.baseUrlBuf;
            st.cfg.apiKey = st.apiKeyBuf;
            st.cfg.model = st.modelBuf;
            st.error = llm::saveConfig(st.cfg) ? "配置已保存（llm_config.json）"
                                               : "配置保存失败（工作目录不可写）";
            st.phase = 3; // 借错误行显示保存结果
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("规约文档 / 自然语言描述：");
    ImGui::InputTextMultiline("##aiinput", st.input, sizeof(st.input),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 8));
    ImGui::TextDisabled("例：「帧头 AA 55，第 3 字节起 2 字节大端为温度，乘 0.1；随后 1 字节为泵状态」");

    if (st.phase == 1) {
        ImGui::TextDisabled("请求中...（最长约 60s）");
    } else {
        if (ImGui::Button("生成", ImVec2(120, 0)) && st.input[0] != '\0') {
            if (st.worker.joinable()) st.worker.join();
            st.cfg.baseUrl = st.baseUrlBuf;
            st.cfg.apiKey = st.apiKeyBuf;
            st.cfg.model = st.modelBuf;
            st.response.clear();
            st.error.clear();
            st.phase = 1;
            std::string system = llm::aiProtoSystemPrompt();
            std::string user = st.input;
            st.worker = std::thread([&st, system, user] {
                std::string resp, err;
                if (!llm::chatCompletion(st.cfg, system, user, resp, err)) {
                    st.error = std::move(err);
                    st.phase = 3;
                    return;
                }
                llm::AiProtoResult r;
                std::string perr;
                if (!llm::parseAiProtoResponse(resp, r, perr)) {
                    st.error = perr.empty() ? "解析失败" : perr;
                    st.phase = 3;
                    return;
                }
                st.response = std::move(resp);
                st.result = std::move(r);
                st.error = std::move(perr);
                st.phase = 2;
            });
        }
    }

    // ---- 结果展示 ----
    if (st.phase == 2) {
        ImGui::Separator();
        const llm::AiProtoResult& r = st.result;
        ImGui::Text("拆帧：%s%s", r.tlv ? "TLV" : "帧头+Length",
                    r.tlv ? "" : ("  帧头 " + r.headerHex).c_str());
        if (!r.fields.empty() && ImGui::BeginTable("aifields", 4,
                                                   ImGuiTableFlags_Borders |
                                                       ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("字段");
            ImGui::TableSetupColumn(r.tlv ? "槽位" : "偏移");
            ImGui::TableSetupColumn("类型");
            ImGui::TableSetupColumn("说明");
            ImGui::TableHeadersRow();
            char buf[96];
            for (const auto& f : r.fields) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.name.c_str());
                ImGui::TableNextColumn();
                if (r.tlv)
                    ImGui::Text("%02X", f.tagId);
                else
                    ImGui::Text("%d", f.offset);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(f.type.c_str());
                ImGui::TableNextColumn();
                if (f.type == "string")
                    ImGui::Text("%d 字节", f.strLen);
                else if (f.type == "enum" && !f.enumItems.empty())
                    ImGui::Text("%d 项映射", (int)f.enumItems.size());
                else if (!r.tlv)
                    ImGui::Text("偏移 %d", f.offset);
                else {
                    snprintf(buf, sizeof(buf), "T=%02X 偏移 %d", f.tagId, f.offset);
                    ImGui::TextUnformatted(buf);
                }
            }
            ImGui::EndTable();
        }
        if (!st.error.empty())
            ImGui::TextDisabled("%s", st.error.c_str());
        if (ImGui::Button("应用到组件", ImVec2(120, 0))) {
            ctx.doc.commit("AI 生成规约");
            llm::applyAiProto(c, st.result);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("应用后可继续手工调整");
    } else if (st.phase == 3 && !st.error.empty()) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", st.error.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("关闭", ImVec2(80, 0))) {
        if (st.worker.joinable()) st.worker.join();
        st.phase = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// 协议组组件的成员编辑区：p<i>.name 索引属性存协议配置组件名（下拉跨页候选）
void drawProtocolGroupMembers(Component& c, PlannerContext& ctx) {
    Component* cp = &c;
    ImGui::Separator();
    ImGui::TextUnformatted("组内协议（按顺序合并字段；拆帧参数取第一个）");
    int64_t count64 = props::asInt(cp->propOr("protoCount", int64_t(0)));
    int count = (int)std::clamp<int64_t>(count64, 0, 32);

    if (ImGui::Button("添加协议")) {
        ctx.doc.commit("协议组添加成员");
        cp->setProp("p" + std::to_string(count) + ".name", std::string());
        cp->setProp("protoCount", int64_t(count + 1));
        ++count;
    }
    ImGui::SameLine();
    if (count > 0 && ImGui::Button("删除末尾")) {
        ctx.doc.commit("协议组移除成员");
        cp->setProp("protoCount", int64_t(count - 1));
    }
    if (count == 0) {
        ImGui::TextDisabled("  (空协议组：数据源关联后无字段)");
        return;
    }
    for (int i = 0; i < count; ++i) {
        std::string p = "p" + std::to_string(i) + ".";
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-60.0f);
        std::string cur = props::asString(cp->propOr(p + "name", std::string()));
        const char* preview = cur.empty() ? "(选择协议配置)" : cur.c_str();
        if (ImGui::BeginCombo("##m", preview)) {
            if (ImGui::Selectable("(清空)", cur.empty())) {
                ctx.doc.commit("协议组成员");
                cp->setProp(p + "name", std::string());
            }
            for (const auto& pg : ctx.project().pages)
                for (const auto& pc : pg.components) {
                    if (pc.typeId != "ProtocolConfig") continue;
                    bool sel = pc.name == cur;
                    if (ImGui::Selectable(pc.name.c_str(), sel) && !sel) {
                        ctx.doc.commit("协议组成员");
                        cp->setProp(p + "name", pc.name);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
}

// 协议配置组件的规约字段编辑区：字段以 f<i>.* 索引属性存储（随组件快照进 undo/序列化）
void drawProtocolFields(Component& c, PlannerContext& ctx) {
    // 指针 + 稳定 id：函数中段「生成字段组件」会 push_back 扩容使组件搬移，
    // 生成后必须重新解析指针（引用无法重绑定），否则后续表格读写悬垂崩溃
    Component* cp = &c;
    ComponentId cId = cp->id;
    ImGui::Separator();
    ImGui::TextUnformatted("规约字段（帧 -> 标签槽位）");
    int64_t count64 = props::asInt(cp->propOr("fieldCount", int64_t(0)));
    int count = (int)std::clamp<int64_t>(count64, 0, 64);

    if (ImGui::Button("添加字段")) {
        ctx.doc.commit("添加规约字段");
        std::string p = "f" + std::to_string(count) + ".";
        cp->setProp(p + "name", std::string("字段" + std::to_string(count + 1)));
        // 槽位(T 值)默认与前一条字段相同（首条为 0），不自增：实际 T 值由报文决定
        int64_t prevTagId = 0;
        if (count > 0)
            prevTagId = props::asInt(
                cp->propOr("f" + std::to_string(count - 1) + ".tagId", int64_t(0)));
        cp->setProp(p + "tagId", prevTagId);
        // 偏移默认接续上一字段末尾（上一字段偏移 + 其字节长度）：定长类型由类型决定，
        // string/enum 由其长度属性决定；首条为 0
        int64_t offset = 0;
        if (count > 0) {
            std::string pp = "f" + std::to_string(count - 1) + ".";
            packet::FieldType prevType = packet::FieldType::U16;
            packet::fieldTypeFromString(
                props::asString(cp->propOr(pp + "type", std::string("u16"))), prevType);
            int bytes = packet::fieldTypeBytes(prevType);
            if (bytes == 0) {
                if (prevType == packet::FieldType::String) {
                    bytes = (int)std::clamp<int64_t>(
                        props::asInt(cp->propOr(pp + "len", int64_t(16))), 1, 256);
                } else { // Enum：宽度限 1/2/4 字节
                    int64_t w = props::asInt(cp->propOr(pp + "len", int64_t(1)));
                    bytes = (w == 2 || w == 4) ? (int)w : 1;
                }
            }
            offset = props::asInt(cp->propOr(pp + "offset", int64_t(0))) + bytes;
        }
        cp->setProp(p + "offset", offset);
        cp->setProp(p + "type", std::string("u16"));
        cp->setProp(p + "bigEndian", true);
        cp->setProp("fieldCount", int64_t(count + 1));
        ++count;
    }
    ImGui::SameLine();
    if (count > 0 && ImGui::Button("删除末尾")) {
        ctx.doc.commit("删除规约字段");
        cp->setProp("fieldCount", int64_t(count - 1));
        --count;
    }
    ImGui::SameLine();
    if (ImGui::Button("AI 配置规约…"))
        ImGui::OpenPopup("AI 配置规约");
    drawAiProtoDialog(*cp, ctx);
    ImGui::SameLine();
    // 一键生成规约字段的显示组件（bool→灯、其余类型→文本，自动 bindField；
    // 逻辑在 base：内部快照协议数据规避 push_back 扩容导致的引用失效崩溃）
    static std::string genStatus;
    {
    if (ImGui::Button("生成字段组件") && count > 0) {
        Page* page = ctx.currentPagePtr();
        if (page) {
            ctx.doc.commit("生成字段组件");
            int skipped = 0;
            auto ids = generateFieldComponents(*page, ctx.project(), cId, skipped);
            cp = page->find(cId); // vector 已扩容搬移，重新解析
            if (!cp) return;
            for (const auto& id : ids)
                ctx.selection.insert(id);
            genStatus = "生成 " + std::to_string(ids.size()) + " 个组件（协议组件下方）";
            if (skipped > 0)
                genStatus += "，跳过 " + std::to_string(skipped) + " 个已绑定";
            if (ids.empty())
                ctx.doc.undo(); // 无变更回退空 commit，避免多余 undo 记录
        }
    }
        if (!genStatus.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", genStatus.c_str());
        }
    }
    bool tlv = props::asString(cp->propOr("framingMode", std::string("TLV"))) == "TLV";
    ImGui::TextDisabled("%s", tlv ? "TLV：字段按槽位(T)匹配帧，偏移相对该帧负载 V"
                                  : "帧头+Length：偏移相对负载（帧头 + length 字段之后）");
    if (count == 0) {
        ImGui::TextDisabled("  (无字段：运行器仍可收帧并监视，但不驱动任何标签)");
        return;
    }

    // 表格式行编辑：列头作标签、控件填满列宽，避免行内控件互相挤压截断
    //（标签槽位由字段序号自动分配，不再编辑；隐式标签按槽位合成）
    int cols = tlv ? 6 : 5;
    if (!ImGui::BeginTable("pfields", cols, ImGuiTableFlags_SizingStretchProp |
                                             ImGuiTableFlags_RowBg))
        return;
    ImGui::TableSetupColumn("字段名", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    if (tlv)
        ImGui::TableSetupColumn("槽位(hex)", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("scale", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("长度/枚举", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < count; ++i) {
        std::string p = "f" + std::to_string(i) + ".";
        ImGui::PushID(i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string name = props::asString(cp->propOr(p + "name", std::string("?")));
        if (ImGui::InputText("##n", &name)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段名");
            cp->setProp(p + "name", name);
        }

        if (tlv) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            int64_t tagId = props::asInt(cp->propOr(p + "tagId", int64_t(0)));
            int v = (int)tagId;
            // 槽位 = TLV 帧 T 值（报文字节），十六进制输入/显示；
            // 取值上限随 T 字节数：1B→0xFF、2B→0xFFFF、3B→0xFFFFFF、4B→0xFFFFFFFF
            int64_t maxTagId = props::asInt(
                cp->propOr("tagBytes", int64_t(1)));
            maxTagId = (int64_t)1 << (int64_t)(8 * std::clamp<int64_t>(maxTagId, 1, 4));
            if (ImGui::InputInt("##t", &v, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段槽位");
                uint32_t uv = (uint32_t)std::clamp<int64_t>(
                    (int64_t)v, 0, std::min<int64_t>(maxTagId - 1, 0xFFFFFFFF));
                cp->setProp(p + "tagId", int64_t(uv));
            }
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        int64_t offset = props::asInt(cp->propOr(p + "offset", int64_t(0)));
        int v = (int)offset;
        if (ImGui::InputInt("##o", &v, 0, 0)) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("字段偏移");
            cp->setProp(p + "offset", int64_t(std::clamp(v, 0, 4096)));
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string typeName = props::asString(cp->propOr(p + "type", std::string("u16")));
        if (ImGui::BeginCombo("##ty", typeName.c_str())) {
            for (int k = 0; k < kFieldTypeCount; ++k) {
                bool sel = typeName == kFieldTypes[k];
                if (ImGui::Selectable(kFieldTypes[k], sel) && !sel) {
                    ctx.doc.commit("字段类型");
                    cp->setProp(p + "type", std::string(kFieldTypes[k]));
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        // 类型可能刚被修改，重新读取以决定本列
        typeName = props::asString(cp->propOr(p + "type", std::string("u16")));

        // scale：数值类型工程换算（工程值 = 原始值 × scale；bool/string/enum 不适用）
        ImGui::TableNextColumn();
        if (typeName != "bool" && typeName != "string" && typeName != "enum") {
            ImGui::SetNextItemWidth(-1);
            double sc = props::asDouble(cp->propOr(p + "scale", 1.0));
            if (ImGui::InputDouble("##sc", &sc, 0.0, 0.0, "%.4f")) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段scale");
                cp->setProp(p + "scale", sc);
            }
        }

        // 长度/枚举：字符串=长度（字节）；枚举=编辑映射；其它=空
        ImGui::TableNextColumn();
        if (typeName == "string") {
            ImGui::SetNextItemWidth(-1);
            int len = (int)props::asInt(cp->propOr(p + "len", int64_t(16)));
            if (ImGui::InputInt("##len", &len, 0, 0)) {
                if (ImGui::IsItemActivated()) ctx.doc.commit("字段长度");
                cp->setProp(p + "len", int64_t(std::clamp(len, 1, 256)));
            }
        } else if (typeName == "enum") {
            if (ImGui::Button("枚举…")) ImGui::OpenPopup("枚举映射");
            ImGui::SetItemTooltip("编辑枚举映射（值 → 名称）与字节宽度");
            if (ImGui::BeginPopup("枚举映射")) {
                drawEnumEditor(c, ctx, p);
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// 数据源组件的「关联协议/组」动态下拉：候选 = 工程内全部协议配置 + 协议组（跨页），
// 二选一：选协议清组、选组清协议（存储仍分 protocol/group 两属性，类型无歧义）
void drawProtocolSelector(Component& c, PlannerContext& ctx) {
    std::string proto = props::asString(c.propOr("protocol", std::string()));
    std::string group = props::asString(c.propOr("group", std::string()));
    // 组优先展示（与生效语义一致）
    std::string preview = !group.empty() ? "[组] " + group
                        : !proto.empty() ? proto
                                         : "(未关联)";
    if (ImGui::BeginCombo("关联协议/组", preview.c_str())) {
        if (ImGui::Selectable("(未关联)", group.empty() && proto.empty())) {
            ctx.doc.commit("取消关联协议");
            c.setProp("protocol", std::string());
            c.setProp("group", std::string());
        }
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "ProtocolConfig") continue;
                bool sel = group.empty() && pc.name == proto;
                if (ImGui::Selectable(pc.name.c_str(), sel) && !sel) {
                    ctx.doc.commit("关联协议");
                    c.setProp("protocol", pc.name);
                    c.setProp("group", std::string());
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        bool anyGroup = false;
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components)
                if (pc.typeId == "ProtocolGroup") anyGroup = true;
        if (anyGroup) ImGui::Separator();
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "ProtocolGroup") continue;
                std::string item = "[组] " + pc.name;
                bool sel = pc.name == group;
                if (ImGui::Selectable(item.c_str(), sel) && !sel) {
                    ctx.doc.commit("关联协议组");
                    c.setProp("group", pc.name);
                    c.setProp("protocol", std::string());
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        ImGui::EndCombo();
    }
}

// 监听=镜像抓包时的「抓包网卡」动态下拉：运行时经 Npcap 枚举本机网卡，
// 显示友好描述、存储 NPF 设备名（跨重启稳定，作为工程持久化标识）。
// 未安装 Npcap 时显示安装提示（保留已存值，便于换机后回来再选）
void drawNicSelector(Component& c, PlannerContext& ctx) {
    std::string cur = props::asString(c.propOr("listenNic", std::string()));
    std::string err;
    auto adapters = packet::npcap::listAdapters(err);
    if (adapters.empty()) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.45f, 1), "抓包网卡: %s", err.c_str());
        return;
    }
    std::string preview = "(未选择)";
    for (const auto& a : adapters)
        if (a.first == cur) preview = a.second.empty() ? a.first : a.second;
    if (preview == "(未选择)" && !cur.empty())
        preview = cur + " (本机不存在)"; // 工程来自其他机器/USB 网卡已拔
    if (ImGui::BeginCombo("抓包网卡", preview.c_str())) {
        for (const auto& a : adapters) {
            std::string label = a.second.empty() ? a.first : a.second;
            bool sel = a.first == cur;
            if (ImGui::Selectable(label.c_str(), sel) && !sel) {
                ctx.doc.commit("选择抓包网卡");
                c.setProp("listenNic", a.first);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// 数据目的组件的「关联数据源」动态下拉：候选 = 工程内全部数据源组件名（跨页）
void drawSourceSelector(Component& c, PlannerContext& ctx) {
    std::string cur = props::asString(c.propOr("source", std::string()));
    const char* preview = cur.empty() ? "(未关联)" : cur.c_str();
    if (ImGui::BeginCombo("关联数据源", preview)) {
        if (ImGui::Selectable("(未关联)", cur.empty())) {
            ctx.doc.commit("取消关联数据源");
            c.setProp("source", std::string());
        }
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "DataSource") continue;
                bool sel = pc.name == cur;
                if (ImGui::Selectable(pc.name.c_str(), sel) && !sel) {
                    ctx.doc.commit("关联数据源");
                    c.setProp("source", pc.name);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
        ImGui::EndCombo();
    }
}

// 按 PropertySpec 生成单个类型化编辑器；返回是否有变更
bool editProperty(const PropertySpec& spec, PropertyValue& value) {
    bool changed = false;
    ImGui::PushID(spec.key.c_str());
    switch (spec.type) {
    case PropertyType::Bool: {
        bool b = props::asBool(value);
        if (ImGui::Checkbox(spec.label.c_str(), &b)) {
            value = b;
            changed = true;
        }
        break;
    }
    case PropertyType::Int: {
        // 直接键入的编辑框（无步进按钮）；越界输入按 spec 范围拉回
        int64_t v = props::asInt(value);
        int i = (int)std::clamp<int64_t>(v, INT_MIN, INT_MAX);
        if (ImGui::InputInt(spec.label.c_str(), &i, 0, 0)) {
            if (spec.minValue) i = std::max(i, (int)*spec.minValue);
            if (spec.maxValue) i = std::min(i, (int)*spec.maxValue);
            value = (int64_t)i;
            changed = true;
        }
        break;
    }
    case PropertyType::Double: {
        double d = props::asDouble(value);
        float f = (float)d;
        float speed = 0.05f * std::max(1.0f, std::abs(f) * 0.01f);
        if (ImGui::DragFloat(spec.label.c_str(), &f, speed,
                             spec.minValue ? (float)*spec.minValue : -FLT_MAX,
                             spec.maxValue ? (float)*spec.maxValue : FLT_MAX, "%.3f")) {
            value = (double)f;
            changed = true;
        }
        break;
    }
    case PropertyType::String: {
        std::string s = props::asString(value);
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText(spec.label.c_str(), buf, sizeof(buf))) {
            value = std::string(buf);
            changed = true;
        }
        break;
    }
    case PropertyType::Color: {
        uint32_t col = props::asColor(value);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
        if (ImGui::ColorEdit4(spec.label.c_str(), &c.x,
                              ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs)) {
            value = ImGui::ColorConvertFloat4ToU32(c);
            changed = true;
        }
        break;
    }
    case PropertyType::Enum: {
        std::string cur = props::asString(value);
        if (ImGui::BeginCombo(spec.label.c_str(), cur.c_str())) {
            for (const auto& opt : spec.enumValues) {
                bool sel = opt == cur;
                if (ImGui::Selectable(opt.c_str(), sel) && !sel) {
                    value = opt;
                    changed = true;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case PropertyType::Vec2: {
        ImVec2 v = props::asVec2(value);
        if (ImGui::DragFloat2(spec.label.c_str(), &v.x)) {
            value = v;
            changed = true;
        }
        break;
    }
    }
    ImGui::PopID();
    return changed;
}

// undo 粒度：连续拖动类控件在"激活首帧"压栈（此刻值尚未修改），离散控件在有变更时压栈
void commitOnEdit(const PropertySpec& spec, bool changed, PlannerContext& ctx) {
    bool discrete = spec.type == PropertyType::Bool || spec.type == PropertyType::Enum ||
                    spec.type == PropertyType::String;
    if (ImGui::IsItemActivated() || (changed && discrete))
        ctx.doc.commit("属性 " + spec.label);
}

// 显示组件的「绑定协议字段」下拉：候选 = 工程内全部协议配置的字段。
// 选择后存组件 bindField（"协议名/字段名"），运行器合成隐式标签+绑定直接驱动
void drawFieldBinding(Component& c, PlannerContext& ctx) {
    ImGui::Separator();
    std::string cur = props::asString(c.propOr("bindField", std::string()));
    const char* preview = cur.empty() ? "(不绑定)" : cur.c_str();
    if (ImGui::BeginCombo("绑定协议字段", preview)) {
        if (ImGui::Selectable("(不绑定)", cur.empty()) && !cur.empty()) {
            ctx.doc.commit("取消绑定协议字段");
            c.setProp("bindField", std::string());
        }
        packet::FramingConfig fr;
        std::vector<TagField> fields;
        for (const auto& pg : ctx.project().pages)
            for (const auto& pc : pg.components) {
                if (pc.typeId != "ProtocolConfig") continue;
                protocolFramingFromComponent(pc, fr, fields);
                for (const auto& f : fields) {
                    // 存储格式 "协议名/字段名"（无空格；解析侧同时容忍带空格的历史数据）
                    std::string item = pc.name + "/" + f.name;
                    bool sel = item == cur;
                    if (ImGui::Selectable(item.c_str(), sel) && !sel) {
                        ctx.doc.commit("绑定协议字段");
                        c.setProp("bindField", item);
                        // 绑定后转复合卡片：保证最小尺寸容纳默认 18 号值
                        //（120x32 的普通标签内容区仅 5px，值会被压到 8 号）
                        if (c.frame.w < 176.0f) c.frame.w = 176.0f;
                        if (c.frame.h < 64.0f) c.frame.h = 64.0f;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(-> %s)", defaultBindableProperty(c));
}

} // namespace

void drawInspector(PlannerContext& ctx) {
    if (!ImGui::Begin("属性")) {
        ImGui::End();
        return;
    }
    Page* page = ctx.currentPagePtr();
    if (!page) {
        ImGui::TextUnformatted("没有页面");
        ImGui::End();
        return;
    }
    if (ctx.selection.empty()) {
        // ---- 页面属性 ----
        ImGui::TextUnformatted("页面属性（未选中组件）");
        ImGui::Separator();
        if (ImGui::InputText("页面名", &page->name)) ctx.doc.commit("页面改名");
        float size[2] = {page->size.x, page->size.y};
        if (ImGui::DragFloat2("尺寸", size, 1.0f, 100.0f, 8192.0f, "%.0f")) {
            if (ImGui::IsItemActivated()) ctx.doc.commit("页面尺寸");
            page->size = ImVec2(std::max(100.0f, size[0]), std::max(100.0f, size[1]));
        }
        // 复合卡片（绑定协议字段的显示组件）左上角字段名的统一字号，随工程保存
        int fs = ctx.project().settings.bindTitleFontSize;
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("字段名字号", &fs, 0, 0, ImGuiInputTextFlags_CharsDecimal)) {
            int clamped = std::clamp(fs, 8, 32);
            if (clamped != ctx.project().settings.bindTitleFontSize) {
                ctx.doc.commit("字段名字号");
                ctx.project().settings.bindTitleFontSize = clamped;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("复合卡片（绑定协议字段的显示组件）左上角字段名的统一字号\n"
                              "对全部复合组件生效，随工程保存");
        ImGui::End();
        return;
    }

    Component* c = page->find(*ctx.selection.begin());
    if (!c) {
        ImGui::TextUnformatted("选中组件不在当前页");
        ImGui::End();
        return;
    }
    const auto* info = ComponentRegistry::instance().find(c->typeId);
    ImGui::Text("%s (%s)", c->name.c_str(), info ? info->displayName.c_str() : "?");
    ImGui::TextDisabled("id: %s", c->id.c_str());
    ImGui::Separator();

    // ---- 名称 ----
    if (ImGui::InputText("名称", &c->name))
        if (ImGui::IsItemDeactivatedAfterEdit()) ctx.doc.commit("改名");

    // ---- 几何与状态 ----
    float pos[2] = {c->frame.x, c->frame.y};
    float size[2] = {c->frame.w, c->frame.h};
    if (ImGui::DragFloat2("位置", pos, 1.0f, -8192.0f, 8192.0f, "%.0f")) {
        if (ImGui::IsItemActivated()) ctx.doc.commit("移动组件");
        c->frame.x = ctx.view.snap(pos[0]);
        c->frame.y = ctx.view.snap(pos[1]);
    }
    if (ImGui::DragFloat2("尺寸", size, 1.0f, 8.0f, 8192.0f, "%.0f")) {
        if (ImGui::IsItemActivated()) ctx.doc.commit("调整尺寸");
        c->frame.w = std::max(8.0f, size[0]);
        c->frame.h = std::max(8.0f, size[1]);
    }
    if (ImGui::Checkbox("可见", &c->visible)) ctx.doc.commit("可见性");
    if (ImGui::Checkbox("锁定", &c->locked)) ctx.doc.commit("锁定");

    // ---- 层级 ----
    ImGui::Separator();
    if (ImGui::Button("置顶")) ctx.nudgeZOrder(10000);
    ImGui::SameLine();
    if (ImGui::Button("上移")) ctx.nudgeZOrder(1);
    ImGui::SameLine();
    if (ImGui::Button("下移")) ctx.nudgeZOrder(-1);
    ImGui::SameLine();
    if (ImGui::Button("置底")) ctx.nudgeZOrder(-10000);

    // ---- 类型化属性 ----
    ImGui::Separator();
    ImGui::TextUnformatted("组件属性");
    if (info) {
        bool isDs = c->typeId == "DataSource";
        bool isSink = c->typeId == "DataSink";
        bool isComm = isDs || isSink; // 通信组件共用传输角色/端口/串口项过滤
        bool isProto = c->typeId == "ProtocolConfig";
        bool isGroup = c->typeId == "ProtocolGroup"; // 组只关联协议，不绑字段
        std::string dsTransport = props::asString(
            c->propOr("transport", std::string(isSink ? "TCP" : "UDP")));
        bool dsTcp = dsTransport == "TCP";
        bool dsSerial = dsTransport == "串口";
        // 监听：固定三元组 dip/dport/协议，绑定端口即 dport——无独立本地监听端口
        bool dsListen = dsTransport == "监听";
        // 监听方式=镜像抓包：Npcap 混杂模式（交换机 SPAN 场景），需选抓包网卡
        bool dsListenPcap =
            dsListen && props::asString(
                            c->propOr("listenMode", std::string("本机端口"))) == "镜像抓包";
        bool dsUdpClient =
            !dsListen && !dsTcp && !dsSerial &&
            props::asString(c->propOr("udpRole", std::string("服务端"))) == "客户端";
        bool dsTcpServer =
            !dsListen && dsTcp &&
            props::asString(c->propOr("tcpRole", std::string("客户端"))) == "服务端";
        // 目标(host/remotePort) 仅客户端使用；本地端口(localPort) 仅服务端使用（监听无此项）
        bool dsUseTarget = dsTcp ? !dsTcpServer : dsUdpClient;
        bool dsUseLocalPort =
            !dsListen && (dsTcp ? dsTcpServer : (!dsUdpClient && !dsSerial));
        bool protoTlv =
            props::asString(c->propOr("framingMode", std::string("TLV"))) == "TLV";
        for (const auto& spec : info->properties) {
            if (isComm) {
                if (spec.key == "protocol" || spec.key == "source" || spec.key == "group")
                    continue; // 动态下拉（候选为协议/数据源/协议组组件名）
                // 按传输方式与客户端/服务端只显示相关项，避免误配：
                // 角色项各自仅对应传输显示；客户端用 host:remotePort；服务端用 localPort；
                // 串口无角色/端口概念，仅显示 serialPort/baud/dataBits/parity/stopBits
                if (spec.key == "udpRole" && (dsTcp || dsSerial || dsListen)) continue;
                if (spec.key == "tcpRole" && (!dsTcp || dsSerial || dsListen)) continue;
                if ((spec.key == "host" || spec.key == "remotePort") && !dsUseTarget) continue;
                if (spec.key == "localPort" && !dsUseLocalPort) continue;
                bool serialItem = spec.key == "serialPort" || spec.key == "baud" ||
                                  spec.key == "dataBits" || spec.key == "parity" ||
                                  spec.key == "stopBits";
                if (serialItem && !dsSerial) continue; // 仅串口
                bool listenItem = spec.key == "listenIp" || spec.key == "listenPort" ||
                                  spec.key == "listenProto";
                if (listenItem && !dsListen) continue; // 仅监听（三元组）
                if (spec.key == "listenMode" && !dsListen) continue;
                if (spec.key == "listenNic") { // 动态下拉（运行时枚举 Npcap 设备）
                    if (!dsListenPcap) continue;
                    drawNicSelector(*c, ctx);
                    continue;
                }
                // 监听下其余传输项已由上面的 dsListen/dsUseTarget/dsUseLocalPort 规则排除，
                // autoStart/transport 照常显示
            }
            if (isProto) { // 拆帧项按模式互斥显示，避免误配
                bool tlvItem = spec.key == "tagBytes" || spec.key == "lenBytes" ||
                               spec.key == "bigEndian" || spec.key == "lenIncludesHeader";
                bool headerItem = spec.key == "headerHex" || spec.key == "lenOffset" ||
                                  spec.key == "lenBytesHeader" ||
                                  spec.key == "bigEndianHeader" || spec.key == "lenIncludesAll";
                if (tlvItem && !protoTlv) continue;
                if (headerItem && protoTlv) continue;
            }
            PropertyValue v = c->propOr(spec.key, spec.defaultValue);
            bool changed = editProperty(spec, v);
            commitOnEdit(spec, changed, ctx);
            if (changed) c->setProp(spec.key, v);
        }
        if (isDs)
            drawProtocolSelector(*c, ctx); // 关联协议/组（单一下拉二选一）
        if (c->typeId == "ProtocolGroup")
            drawProtocolGroupMembers(*c, ctx); // 组内协议成员（索引属性）
        if (isSink)
            drawSourceSelector(*c, ctx); // 关联数据源（动态候选）
        if (isProto)
            drawProtocolFields(*c, ctx); // 规约字段列表（索引属性，自定义编辑）
        if (!isComm && !isProto && !isGroup)
            drawFieldBinding(*c, ctx); // 显示组件直接绑定协议字段
    } else {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.6f, 1), "未注册类型: %s", c->typeId.c_str());
    }

    ImGui::End();
}

} // namespace pv::planner::panels
