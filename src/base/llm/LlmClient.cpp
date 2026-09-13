#include "base/llm/LlmClient.h"

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <fstream>

#pragma comment(lib, "winhttp.lib")

namespace softg::llm {

namespace {

// baseUrl -> scheme/host/port/path（WinHTTP 需要）
bool splitUrl(const std::string& url, bool& secure, std::wstring& host, int& port,
              std::wstring& path, std::string& err) {
    std::string rest = url;
    secure = false;
    if (rest.rfind("https://", 0) == 0) {
        secure = true;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    } else {
        err = "Base URL 需以 http:// 或 https:// 开头";
        return false;
    }
    size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? L"/"
                                    : std::wstring(rest.begin() + slash, rest.end());
    port = secure ? 443 : 80;
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        port = atoi(hostPort.c_str() + colon + 1);
        hostPort = hostPort.substr(0, colon);
    }
    if (hostPort.empty()) {
        err = "Base URL 缺少主机名";
        return false;
    }
    host = std::wstring(hostPort.begin(), hostPort.end());
    return true;
}

std::wstring utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string wideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

bool loadConfig(LlmConfig& cfg) {
    std::ifstream f("llm_config.json");
    if (!f) return false;
    try {
        nlohmann::json j;
        f >> j;
        cfg.baseUrl = j.value("baseUrl", cfg.baseUrl);
        cfg.apiKey = j.value("apiKey", cfg.apiKey);
        cfg.model = j.value("model", cfg.model);
        return true;
    } catch (...) {
        return false;
    }
}

bool saveConfig(const LlmConfig& cfg) {
    nlohmann::json j;
    j["baseUrl"] = cfg.baseUrl;
    j["apiKey"] = cfg.apiKey;
    j["model"] = cfg.model;
    std::ofstream f("llm_config.json", std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return true;
}

bool chatCompletion(const LlmConfig& cfg, const std::string& system,
                    const std::string& user, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    if (cfg.apiKey.empty()) {
        err = "未配置 API Key（在 LLM 设置中填写并保存）";
        return false;
    }

    // 请求体（OpenAI 兼容）
    nlohmann::json body;
    body["model"] = cfg.model;
    body["messages"] = nlohmann::json::array({
        nlohmann::json{{"role", "system"}, {"content", system}},
        nlohmann::json{{"role", "user"}, {"content", user}},
    });
    body["temperature"] = 0.1; // 配置类任务取低随机性
    std::string req = body.dump();

    bool secure = false;
    std::wstring host, path;
    int port = 0;
    if (!splitUrl(cfg.baseUrl, secure, host, port, path, err)) return false;

    HINTERNET session = WinHttpOpen(L"SoftG/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        err = "WinHttpOpen 失败";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000); // 解析/连接/发送/接收

    bool ok = false;
    HINTERNET connect = nullptr, request = nullptr;
    do {
        connect = WinHttpConnect(session, host.c_str(), (INTERNET_PORT)port, 0);
        if (!connect) {
            err = "连接主机失败: " + wideToUtf8(host);
            break;
        }
        request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
        if (!request) {
            err = "创建请求失败";
            break;
        }
        std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " +
                               utf8ToWide(cfg.apiKey) + L"\r\n";
        if (!WinHttpSendRequest(request, headers.c_str(), (DWORD)-1,
                                (void*)req.data(), (DWORD)req.size(), (DWORD)req.size(), 0)) {
            err = "发送请求失败 (网络不可达或代理拦截)";
            break;
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
            err = "等待应答失败";
            break;
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        std::string resp;
        char buf[8192];
        DWORD read = 0;
        while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0)
            resp.append(buf, buf + read);
        if (status < 200 || status >= 300) {
            // 带出响应体片段便于定位（key 无效/额度不足等）
            err = "HTTP " + std::to_string(status) + ": " +
                  resp.substr(0, resp.size() > 300 ? 300 : resp.size());
            break;
        }
        try {
            auto j = nlohmann::json::parse(resp);
            out = j.at("choices").at(0).at("message").at("content").get<std::string>();
            ok = true;
        } catch (const std::exception& e) {
            err = std::string("应答解析失败: ") + e.what();
        }
    } while (false);

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

} // namespace softg::llm
