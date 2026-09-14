// LlmClient — OpenAI 兼容 chat/completions 客户端（WinHTTP，支持 http/https）。
// 供「AI 配置规约」调用：用户输入规约文档/自然语言 -> LLM 生成字段配置。
// 同步接口：调用方在后台线程执行（UI 侧需异步包装）。
#pragma once

#include <string>

namespace pv::llm {

struct LlmConfig {
    std::string baseUrl = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
    std::string apiKey;                   // 用户自行填入（本地明文保存）
    std::string model = "glm-4-flash";
};

// 配置持久化（exe 工作目录 llm_config.json）
bool loadConfig(LlmConfig& cfg);
bool saveConfig(const LlmConfig& cfg);

// 同步对话补全：system+user -> 返回首个 choice 的 content 文本。
// 失败返回 false，err 给出原因（网络/HTTP 状态/解析）。
bool chatCompletion(const LlmConfig& cfg, const std::string& system,
                    const std::string& user, std::string& out, std::string& err);

} // namespace pv::llm
