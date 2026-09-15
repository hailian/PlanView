#include "planner/panels/Validation.h"

#include "base/packet/UsbLink.h" // parseDeviceToken（USB 设备标识校验）
#include "imgui.h"
#include "planner/PlannerContext.h"

namespace pv::planner::panels {

void drawValidation(PlannerContext& ctx) {
    if (!ImGui::Begin("校验")) {
        ImGui::End();
        return;
    }
    Project& p = ctx.project();
    struct Issue {
        std::string text;
        ComponentId jumpTo;  // 非空时双击跳转
        bool jumpable = false;
    };
    std::vector<Issue> issues;

    auto checkComponent = [&](const ComponentId& id, const std::string& what) {
        if (!p.findComponent(id)) issues.push_back({what + " 引用了不存在的组件: " + id, id, true});
    };

    for (const auto& a : p.associations) {
        if (auto* b = std::get_if<DataBinding>(&a)) {
            checkComponent(b->component, "数据绑定");
            if (!p.tags.find(b->tag))
                issues.push_back({"数据绑定引用了不存在的标签: " + b->tag, "", false});
        } else if (auto* l = std::get_if<LinkageRule>(&a)) {
            checkComponent(l->source, "联动源");
            if (l->action == LinkageAction::Navigate) {
                if (!p.findPage(l->param))
                    issues.push_back({"联动跳转目标页面不存在: " + l->param, "", false});
            } else if (l->action == LinkageAction::SetTagValue) {
                if (!p.tags.find(l->param))
                    issues.push_back({"联动写值目标标签不存在: " + l->param, "", false});
            } else {
                if (l->target.empty())
                    issues.push_back({"联动缺少目标组件", l->source, true});
                else
                    checkComponent(l->target, "联动目标");
            }
        } else if (auto* r = std::get_if<AlarmRule>(&a)) {
            if (!p.tags.find(r->tag))
                issues.push_back({"告警规则引用了不存在的标签: " + r->tag, "", false});
            for (const auto& cid : r->components)
                checkComponent(cid, "告警受影响");
        }
    }
    // 无任何绑定的标签（提示级）
    for (const auto& t : p.tags.all()) {
        bool used = false;
        for (const auto& a : p.associations) {
            if (auto* b = std::get_if<DataBinding>(&a); b && b->tag == t.name) used = true;
            if (auto* r = std::get_if<AlarmRule>(&a); r && r->tag == t.name) used = true;
            if (auto* l = std::get_if<LinkageRule>(&a); l && l->param == t.name) used = true;
        }
        if (!used)
            issues.push_back({"标签未被任何关联引用: " + t.name + " @" + std::to_string(t.address), "",
                              false});
    }

    // 数据源组件全局唯一：多个时仅第一个生效（findComponentByType 语义）
    std::vector<const Component*> protocols; // 协议配置组件（重名检测用）
    {
        int dsCount = 0;
        int autoStartCount = 0;
        const Component* firstDs = nullptr;
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components)
                if (c.typeId == "DataSource") {
                    if (!firstDs) firstDs = &c;
                    ++dsCount;
                    if (props::asBool(c.propOr("autoStart", false))) ++autoStartCount;
                } else if (c.typeId == "ProtocolConfig") {
                    protocols.push_back(&c);
                }
        if (dsCount > 1)
            issues.push_back({"数据源组件超过一个（仅第一个生效）: " +
                                  std::to_string(dsCount) + " 个",
                              firstDs ? firstDs->id : "", true});
        if (firstDs && autoStartCount == 0)
            issues.push_back({"数据源未设自动启动（PageViewer 打开后在顶栏手动启动数据源）",
                              firstDs->id, false});
        // 数据源关联的协议/协议组必须存在（按名称匹配；绑组时不再要求直连协议）
        if (firstDs) {
            std::string protoName =
                props::asString(firstDs->propOr("protocol", std::string()));
            std::string groupName = props::asString(firstDs->propOr("group", std::string()));
            if (!protoName.empty()) {
                bool found = false;
                for (const auto* pc : protocols)
                    if (pc->name == protoName) found = true;
                if (!found)
                    issues.push_back({"数据源关联的协议配置不存在: " + protoName,
                                      firstDs->id, true});
            } else if (groupName.empty()) {
                issues.push_back({"数据源未关联协议配置（默认 TLV，无字段）", firstDs->id,
                                  true});
            }
            // groupName 非空：组存在性由下方「数据源关联的协议组不存在」检查，
            // 组内成员由「协议组包含的协议不存在」检查，此处不重复报未关联

            // 监听=镜像抓包须选择抓包网卡（Npcap 是否安装由运行器连接时报错提示）
            if (props::asString(firstDs->propOr("transport", std::string("UDP"))) == "监听" &&
                props::asString(firstDs->propOr("listenMode", std::string("本机端口"))) ==
                    "镜像抓包" &&
                props::asString(firstDs->propOr("listenNic", std::string())).empty())
                issues.push_back({"镜像抓包未选择抓包网卡（运行器无法启动监听）",
                                  firstDs->id, true});

            // USB 传输须选择合法设备标识（libusb DLL 是否放置由运行器连接时报错提示）
            if (props::asString(firstDs->propOr("transport", std::string("UDP"))) == "USB") {
                std::string dev =
                    props::asString(firstDs->propOr("usbDevice", std::string()));
                uint16_t vid = 0, pid = 0;
                std::string serial, perr;
                if (dev.empty())
                    issues.push_back({"USB 传输未选择 USB 设备（运行器无法启动收包）",
                                      firstDs->id, true});
                else if (!packet::parseDeviceToken(dev, vid, pid, serial, perr))
                    issues.push_back({"USB 设备标识非法（" + perr + "）", firstDs->id, true});
            }
        }

        // 数据源关联的协议组须存在；组内成员协议须存在
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components) {
                if (c.typeId == "ProtocolGroup") {
                    int64_t n = props::asInt(c.propOr("protoCount", int64_t(0)));
                    for (int64_t i = 0; i < n; ++i) {
                        std::string member = props::asString(
                            c.propOr("p" + std::to_string(i) + ".name", std::string()));
                        if (member.empty()) continue;
                        bool found = false;
                        for (const auto* pc : protocols)
                            if (pc->name == member) found = true;
                        if (!found)
                            issues.push_back({"协议组包含的协议不存在: " + member, c.id,
                                              true});
                    }
                    continue;
                }
                if (c.typeId != "DataSource") continue;
                std::string g = props::asString(c.propOr("group", std::string()));
                if (g.empty()) continue;
                bool found = false;
                for (const auto& pg2 : p.pages)
                    for (const auto& c2 : pg2.components)
                        if (c2.typeId == "ProtocolGroup" && c2.name == g) found = true;
                if (!found)
                    issues.push_back({"数据源关联的协议组不存在: " + g, c.id, true});
            }

        // 数据目的：关联的数据源必须存在（按名称匹配）
        for (const auto& pg : p.pages)
            for (const auto& c : pg.components) {
                if (c.typeId != "DataSink") continue;
                std::string srcName = props::asString(c.propOr("source", std::string()));
                if (srcName.empty()) {
                    issues.push_back({"数据目的未关联数据源（不转发）", c.id, true});
                    continue;
                }
                bool found = false;
                for (const auto& pg2 : p.pages)
                    for (const auto& c2 : pg2.components)
                        if (c2.typeId == "DataSource" && c2.name == srcName) found = true;
                if (!found)
                    issues.push_back({"数据目的关联的数据源不存在: " + srcName, c.id, true});
                else if (firstDs && srcName != firstDs->name)
                    issues.push_back({"数据目的关联的不是首个（生效）数据源: " + srcName,
                                      c.id, true});
                // USB 转发须选择合法设备标识（与数据源同规则）
                if (props::asString(c.propOr("transport", std::string("TCP"))) == "USB") {
                    std::string dev = props::asString(c.propOr("usbDevice", std::string()));
                    uint16_t vid = 0, pid = 0;
                    std::string serial, perr;
                    if (dev.empty())
                        issues.push_back({"USB 转发未选择 USB 设备（不转发）", c.id, true});
                    else if (!packet::parseDeviceToken(dev, vid, pid, serial, perr))
                        issues.push_back({"USB 设备标识非法（" + perr + "）", c.id, true});
                }
            }
    }
    // 协议配置：重名（关联按名称匹配会歧义）与未被引用提示
    if (!protocols.empty()) {
        for (size_t i = 0; i < protocols.size(); ++i) {
            for (size_t j = i + 1; j < protocols.size(); ++j)
                if (protocols[i]->name == protocols[j]->name)
                    issues.push_back({"协议配置重名（关联按名称匹配，仅第一个生效）: " +
                                          protocols[i]->name,
                                      protocols[i]->id, true});
        }
        for (const auto* pc : protocols) {
            bool referenced = false;
            for (const auto& pg : p.pages)
                for (const auto& c : pg.components) {
                    if (c.typeId == "DataSource" &&
                        props::asString(c.propOr("protocol", std::string())) == pc->name)
                        referenced = true;
                    // 被协议组包含即视为已关联（组再由数据源使用），无需直连数据源
                    if (c.typeId == "ProtocolGroup") {
                        int64_t n = props::asInt(c.propOr("protoCount", int64_t(0)));
                        for (int64_t i = 0; i < n; ++i)
                            if (props::asString(c.propOr("p" + std::to_string(i) + ".name",
                                                         std::string())) == pc->name)
                                referenced = true;
                    }
                }
            if (!referenced)
                issues.push_back({"协议配置未被任何数据源关联: " + pc->name, pc->id, true});
        }
    }

    // 组件 bindField（直接绑定协议字段）引用检查
    for (const auto& pg : p.pages)
        for (const auto& c : pg.components) {
            if (c.typeId == "DataSource" || c.typeId == "ProtocolConfig") continue;
            std::string bf = props::asString(c.propOr("bindField", std::string()));
            if (bf.empty()) continue;
            size_t slash = bf.find('/');
            if (slash == std::string::npos) {
                issues.push_back({"组件字段绑定格式非法（应为 协议名/字段名）: " + bf, c.id,
                                  true});
                continue;
            }
            auto trim = [](const std::string& s) {
                size_t b = s.find_first_not_of(" \t");
                size_t e = s.find_last_not_of(" \t");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            };
            std::string protoName = trim(bf.substr(0, slash));
            std::string fieldName = trim(bf.substr(slash + 1));
            const Component* pc = nullptr;
            for (const auto& pg2 : p.pages)
                for (const auto& c2 : pg2.components)
                    if (c2.typeId == "ProtocolConfig" && c2.name == protoName) pc = &c2;
            if (!pc) {
                issues.push_back({"组件绑定的协议不存在: " + protoName, c.id, true});
                continue;
            }
            bool found = false;
            for (int64_t i = 0;
                 i < props::asInt(pc->propOr("fieldCount", int64_t(0))) && !found; ++i) {
                std::string pfx = "f" + std::to_string(i) + ".";
                found = props::asString(pc->propOr(pfx + "name", std::string())) == fieldName;
            }
            if (!found)
                issues.push_back({"组件绑定的协议字段不存在: " + bf, c.id, true});
        }

    if (issues.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "未发现问题 (%d 项关联, %d 个标签)",
                           (int)p.associations.size(), (int)p.tags.all().size());
    } else {
        ImGui::Text("发现 %d 项问题:", (int)issues.size());
        int n = 0;
        for (const auto& iss : issues) {
            ImGui::PushID(n++);
            bool jump = iss.jumpable && !iss.jumpTo.empty();
            if (ImGui::Selectable(iss.text.c_str(), false,
                                  jump ? ImGuiSelectableFlags_AllowDoubleClick : 0)) {
                if (jump && ImGui::IsMouseDoubleClicked(0)) {
                    ctx.focusComponent = iss.jumpTo;
                    ctx.focusRequested = true;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::TextDisabled("双击可定位的问题行跳转到相关组件");
    ImGui::End();
}

} // namespace pv::planner::panels
