// TCP 数据源：行文本协议客户端（Winsock）。实现 IDataSource。
// 500ms 连接超时、SO_RCVTIMEO 收发超时、断线由调用方（PollWorker）重连。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "base/data/IDataSource.h"

namespace softg::tcp {

class TcpDataSource : public IDataSource {
public:
    explicit TcpDataSource(TcpSettings settings);
    ~TcpDataSource() override;

    bool connect(std::string& err) override;
    void disconnect() override;
    bool isConnected() const override { return sock_ != kInvalidSocketValue; }

    std::vector<TagReadResult> readTags(const std::vector<const Tag*>& tags) override;
    bool writeTag(const Tag& tag, TagValue value, std::string& err) override;

    // 供单测注入超时
    int connectTimeoutMs = 500;
    int ioTimeoutMs = 1000;

private:
    // 发送一行并等待一行应答（处理 TCP 分段重组）
    bool transactLine(const std::string& req, std::string& resp, std::string& err);
    bool recvLine(std::string& line, std::string& err);

    static constexpr uintptr_t kInvalidSocketValue = (uintptr_t)-1;

    TcpSettings settings_;
    uintptr_t sock_ = kInvalidSocketValue;  // SOCKET
    std::string pending_;                   // 已收未满一行的字节
};

} // namespace softg::tcp
