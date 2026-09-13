// M9 单测：AI 规约生成 — LLM 响应解析（围栏容错/类型校验/枚举与字符串）+ 应用到协议组件
#include "SoftgTest.h"

#include "base/llm/AiProto.h"
#include "base/model/ComponentRegistry.h"
#include "base/data/frame/FrameSourceSettings.h"

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
