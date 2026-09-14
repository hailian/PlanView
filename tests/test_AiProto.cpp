// M9 单测：AI 规约生成 — LLM 响应解析（围栏容错/类型校验/枚举与字符串）+ 应用到协议组件
#include "SoftgTest.h"

#include "base/llm/AiProto.h"
#include "base/data/frame/FrameSourceSettings.h"
#include "base/model/ComponentRegistry.h"
#include "base/model/Project.h"

using namespace softg;

TEST_CASE("AI 规约：响应解析（带围栏 + 帧头+Length + string/enum）") {
    const char* raw =
        "好的，以下是配置：\n"
        "```json\n"
        "{\n"
        "  \"framingMode\": \"帧头+Length\",\n"
        "  \"headerHex\": \"EB 90\",\n"
        "  \"lenOffset\": 3,\n"
        "  \"lenBytesHeader\": 1,\n"
        "  \"bigEndianHeader\": false,\n"
        "  \"lenIncludesAll\": false,\n"
        "  \"fields\": [\n"
        "    {\"name\": \"温度\", \"offset\": 0, \"type\": \"f32\", \"bigEndian\": true},\n"
        "    {\"name\": \"状态\", \"offset\": 4, \"type\": \"enum\", \"enum\": "
        "[{\"v\":0,\"n\":\"停止\"},{\"v\":1,\"n\":\"运行\"}]},\n"
        "    {\"name\": \"编号\", \"offset\": 5, \"type\": \"string\", \"len\": 8},\n"
        "    {\"name\": \"坏类型\", \"offset\": 13, \"type\": \"decimal\"}\n"
        "  ]\n"
        "}\n"
        "```\n";
    llm::AiProtoResult r;
    std::string err;
    REQUIRE(llm::parseAiProtoResponse(raw, r, err));
    CHECK(!r.tlv);
    CHECK(r.headerHex == "EB 90");
    CHECK(r.lenOffset == 3);
    CHECK(r.lenBytesHeader == 1);
    CHECK(!r.bigEndianHeader);
    CHECK(r.fields.size() == 4);
    CHECK(r.fields[0].name == "温度");
    CHECK(r.fields[0].type == "f32");
    CHECK(r.fields[0].offset == 0);
    CHECK(r.fields[1].type == "enum");
    CHECK(r.fields[1].enumItems.size() == 2);
    CHECK(r.fields[1].enumItems[1].value == 1);
    CHECK(r.fields[1].enumItems[1].name == "运行");
    CHECK(r.fields[2].type == "string");
    CHECK(r.fields[2].strLen == 8);
    CHECK(r.fields[3].type == "u16"); // 非法类型回退
}

TEST_CASE("AI 规约：TLV 响应 + 应用到协议组件（读回一致）") {
    const char* raw =
        "{\"framingMode\":\"TLV\",\"tagBytes\":1,\"lenBytes\":2,\"bigEndian\":true,"
        "\"lenIncludesHeader\":false,"
        "\"fields\":["
        "{\"name\":\"温度\",\"tagId\":1,\"offset\":0,\"type\":\"u16\"},"
        "{\"name\":\"泵\",\"tagId\":2,\"offset\":0,\"type\":\"bool\"},"
        "{\"name\":\"模式\",\"tagId\":3,\"offset\":0,\"type\":\"enum\","
        "\"enum\":[{\"v\":0,\"n\":\"手动\"},{\"v\":1,\"n\":\"自动\"}]}"
        "]}";
    llm::AiProtoResult r;
    std::string err;
    REQUIRE(llm::parseAiProtoResponse(raw, r, err));
    CHECK(r.tlv);
    CHECK(r.tagBytes == 1);
    CHECK(r.fields.size() == 3);

    Component c = ComponentRegistry::createComponent("ProtocolConfig", "pc-1");
    c.name = "AI生成协议";
    llm::applyAiProto(c, r);

    // 经协议解析路径读回验证
    packet::FramingConfig fr;
    std::vector<TagField> fields;
    protocolFramingFromComponent(c, fr, fields);
    CHECK(fr.mode == packet::FrameMode::Tlv);
    CHECK(fr.tagBytes == 1);
    CHECK(fr.lenBytes == 2);
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].name == "温度");
    CHECK(fields[0].tagId == 1);
    CHECK(props::asString(c.propOr("f0.type", std::string())) == "u16");
    CHECK(props::asString(c.propOr("f1.type", std::string())) == "bool");
    CHECK(props::asString(c.propOr("f2.type", std::string())) == "enum");
    CHECK(props::asInt(c.propOr("f2.enumCount", int64_t(0))) == 2);
    CHECK(props::asString(c.propOr("f2.e1.n", std::string())) == "自动");
    CHECK(fields[0].address == 0); // 槽位 = 字段序号
    CHECK(fields[1].address == 1);
    CHECK(fields[2].address == 2);
}

TEST_CASE("AI 规约：非法响应与空字段") {
    llm::AiProtoResult r;
    std::string err;
    CHECK(!llm::parseAiProtoResponse("没有任何 JSON", r, err));
    CHECK(!llm::parseAiProtoResponse("{\"a\":", r, err));
    CHECK(llm::parseAiProtoResponse("{\"framingMode\":\"TLV\"}", r, err));
    CHECK(r.fields.empty());
    CHECK(err.find("未解析到字段") == 0);
}

TEST_CASE("一键生成字段组件：枚举/浮点/字符串三字段（回归：push_back 扩容悬垂崩溃）") {
    Project p;
    Page pg;
    pg.id = "page-1";
    p.pages.push_back(std::move(pg));

    Component proto = ComponentRegistry::createComponent("ProtocolConfig", "pc-1");
    proto.name = "设备协议";
    proto.setProp("fieldCount", int64_t(5));
    proto.setProp("f0.name", std::string("设备1状态"));
    proto.setProp("f0.type", std::string("enum"));
    proto.setProp("f0.len", int64_t(1));
    proto.setProp("f0.enumCount", int64_t(2));
    proto.setProp("f0.e0.v", int64_t(0));
    proto.setProp("f0.e0.n", std::string("关机"));
    proto.setProp("f0.e1.v", int64_t(1));
    proto.setProp("f0.e1.n", std::string("开机"));
    proto.setProp("f1.name", std::string("设备1温度"));
    proto.setProp("f1.type", std::string("f32"));
    proto.setProp("f2.name", std::string("设备1位置"));
    proto.setProp("f2.type", std::string("string"));
    proto.setProp("f2.len", int64_t(4));
    proto.setProp("f3.name", std::string("设备1累计量"));
    proto.setProp("f3.type", std::string("f64"));
    proto.setProp("f4.name", std::string("设备1转速"));
    proto.setProp("f4.type", std::string("u16"));
    p.pages[0].components.push_back(proto);
    ComponentId protoId = p.pages[0].components[0].id;

    int skipped = 0;
    auto ids = generateFieldComponents(p.pages[0], p, protoId, skipped);
    CHECK(skipped == 0);
    REQUIRE(ids.size() == 5); // 仅显示组件（字段名由绑定卡片自显示）
    REQUIRE(p.pages[0].components.size() == 6);

    // 类型映射：仅 bool→Lamp，其余（enum/string/f32/f64/整数）一律→Label；bindField 自动绑定
    const Component* c1 = p.pages[0].find(ids[0]);
    const Component* c2 = p.pages[0].find(ids[1]);
    const Component* c3 = p.pages[0].find(ids[2]);
    const Component* c4 = p.pages[0].find(ids[3]);
    const Component* c5 = p.pages[0].find(ids[4]);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    REQUIRE(c3 != nullptr);
    REQUIRE(c4 != nullptr);
    REQUIRE(c5 != nullptr);
    CHECK(c1->typeId == "Label");
    CHECK(c1->name == "设备1状态");
    CHECK(c1->frame.w == 176.0f && c1->frame.h == 64.0f); // 生成默认尺寸（复合卡片）
    CHECK(props::asString(c1->propOr("bindField", std::string())) == "设备协议/设备1状态");
    CHECK(c2->typeId == "Label"); // f32 浮点同样生成文本
    CHECK(props::asString(c2->propOr("bindField", std::string())) == "设备协议/设备1温度");
    CHECK(c3->typeId == "Label");
    CHECK(props::asString(c3->propOr("bindField", std::string())) == "设备协议/设备1位置");
    CHECK(c4->typeId == "Label");
    CHECK(props::asString(c4->propOr("bindField", std::string())) == "设备协议/设备1累计量");
    CHECK(c5->typeId == "Label"); // 整数同样生成文本（仪表不再用于一键生成）
    CHECK(props::asString(c5->propOr("bindField", std::string())) == "设备协议/设备1转速");
    CHECK(c2->frame.w == 176.0f && c2->frame.h == 64.0f);

    // 纵向排列：行 y 严格递增；不再生成独立的字段名文本
    for (int k = 1; k < 5; ++k) {
        const Component* prev = p.pages[0].find(ids[k - 1]);
        const Component* cur = p.pages[0].find(ids[k]);
        REQUIRE(prev != nullptr);
        REQUIRE(cur != nullptr);
        CHECK(cur->frame.y > prev->frame.y);
    }

    // 协议组件数据在多次 push_back 后仍完好（快照实现下读回一致）
    const Component* pc = p.pages[0].find(protoId);
    REQUIRE(pc != nullptr);
    CHECK(props::asInt(pc->propOr("fieldCount", int64_t(0))) == 5);
    CHECK(props::asString(pc->propOr("f2.name", std::string())) == "设备1位置");
    CHECK(props::asString(pc->propOr("f0.e1.n", std::string())) == "开机");

    // 幂等：再次生成全部跳过
    int skipped2 = 0;
    auto ids2 = generateFieldComponents(p.pages[0], p, protoId, skipped2);
    CHECK(ids2.empty());
    CHECK(skipped2 == 5);
    CHECK(p.pages[0].components.size() == 6);
}
