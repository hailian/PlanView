#include "base/data/tcp/TcpDataSource.h"

#include "base/data/tcp/PvProtocol.h"
#include "base/log/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace pv::tcp {

TcpDataSource::TcpDataSource(TcpSettings settings) : settings_(std::move(settings)) {}

TcpDataSource::~TcpDataSource() { disconnect(); }

bool TcpDataSource::connect(std::string& err) {
    disconnect();

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket 创建失败";
        return false;
    }

    // 非阻塞连接 + select 超时
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)settings_.port);
    inet_pton(AF_INET, settings_.host.c_str(), &addr.sin_addr);

    int rc = ::connect(s, (sockaddr*)&addr, sizeof(addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet, errSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errSet);
        FD_SET(s, &writeSet);
        FD_SET(s, &errSet);
        timeval tv{connectTimeoutMs / 1000, (connectTimeoutMs % 1000) * 1000};
        rc = ::select(0, nullptr, &writeSet, &errSet, &tv);
        if (rc <= 0) {
            err = "连接超时: " + settings_.host + ":" + std::to_string(settings_.port);
            closesocket(s);
            return false;
        }
        if (FD_ISSET(s, &errSet)) {
            err = "连接被拒绝: " + settings_.host + ":" + std::to_string(settings_.port);
            closesocket(s);
            return false;
        }
    } else if (rc == SOCKET_ERROR) {
        err = "连接失败 (WSA=" + std::to_string(WSAGetLastError()) + "): " + settings_.host;
        closesocket(s);
        return false;
    }

    u_long block = 0;
    ioctlsocket(s, FIONBIO, &block);
    DWORD tv = (DWORD)ioTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

    sock_ = (uintptr_t)s;
    pending_.clear();
    PV_LOG_INFO("TCP 已连接 %s:%d", settings_.host.c_str(), settings_.port);
    return true;
}

void TcpDataSource::disconnect() {
    if (sock_ != kInvalidSocketValue) {
        closesocket((SOCKET)sock_);
        sock_ = kInvalidSocketValue;
    }
}

bool TcpDataSource::recvLine(std::string& line, std::string& err) {
    while (true) {
        size_t nl = pending_.find('\n');
        if (nl != std::string::npos) {
            line = pending_.substr(0, nl);
            pending_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        char buf[1024];
        int n = ::recv((SOCKET)sock_, buf, sizeof(buf), 0);
        if (n <= 0) {
            err = "接收失败/超时 (WSA=" + std::to_string(WSAGetLastError()) + ")";
            disconnect();
            return false;
        }
        pending_.append(buf, (size_t)n);
        if (pending_.size() > 64 * 1024) {  // 防御恶意流
            err = "应答过长";
            disconnect();
            return false;
        }
    }
}

bool TcpDataSource::transactLine(const std::string& req, std::string& resp, std::string& err) {
    if (sock_ == kInvalidSocketValue) {
        err = "未连接";
        return false;
    }
    if (::send((SOCKET)sock_, req.data(), (int)req.size(), 0) == SOCKET_ERROR) {
        err = "发送失败 (WSA=" + std::to_string(WSAGetLastError()) + ")";
        disconnect();
        return false;
    }
    return recvLine(resp, err);
}

std::vector<TagReadResult> TcpDataSource::readTags(const std::vector<const Tag*>& tags) {
    std::vector<TagReadResult> results(tags.size());
    for (size_t i = 0; i < tags.size(); ++i)
        results[i].tag = tags[i] ? tags[i]->name : "";

    auto failAll = [&](const std::string& err, TagQuality q) {
        for (auto& r : results) {
            r.ok = false;
            r.quality = q;
            r.error = err;
        }
    };
    if (sock_ == kInvalidSocketValue) {
        failAll("未连接", TagQuality::CommLost);
        return results;
    }

    std::vector<int> indices;
    indices.reserve(tags.size());
    for (const Tag* t : tags)
        if (t) indices.push_back(t->address);

    std::string resp, err;
    if (!transactLine(encodeReadReq(indices), resp, err)) {
        failAll(err, TagQuality::CommLost);
        return results;
    }
    std::vector<ReadItem> items;
    if (!parseValuesLine(resp, items)) {
        failAll("应答格式非法: " + resp, TagQuality::Bad);
        return results;
    }

    for (size_t i = 0; i < tags.size(); ++i) {
        const Tag* t = tags[i];
        if (!t) continue;
        TagReadResult& r = results[i];
        // 找到本槽位的应答项
        const ReadItem* found = nullptr;
        for (const auto& it : items)
            if (it.index == t->address) { found = &it; break; }
        if (!found || !found->ok) {
            r.ok = false;
            r.quality = TagQuality::Bad;
            r.error = found ? "槽位无数据" : "应答缺少槽位";
            continue;
        }
        // 类型按标签定义转换（服务器类型名一致）
        double raw = 0;
        if (auto* b = std::get_if<bool>(&found->value)) raw = *b ? 1 : 0;
        else if (auto* v = std::get_if<int64_t>(&found->value)) raw = (double)*v;
        else if (auto* d = std::get_if<double>(&found->value)) raw = *d;

        if (t->type == TagDataType::Bool) {
            r.value = raw != 0;
        } else {
            double eng = t->toEngineering(raw);
            r.value = (eng == std::floor(eng) && std::abs(eng) < 9.0e15) ? TagValue((int64_t)eng)
                                                                         : TagValue(eng);
        }
        r.ok = true;
        r.quality = TagQuality::Good;
    }
    return results;
}

bool TcpDataSource::writeTag(const Tag& tag, TagValue value, std::string& err) {
    if (sock_ == kInvalidSocketValue) {
        err = "未连接";
        return false;
    }
    // 工程值 -> 原始值
    double eng = 0;
    if (auto* b = std::get_if<bool>(&value)) eng = *b ? 1 : 0;
    else if (auto* i = std::get_if<int64_t>(&value)) eng = (double)*i;
    else if (auto* d = std::get_if<double>(&value)) eng = *d;
    else {
        err = "写入值必须是数值";
        return false;
    }
    double raw = tag.scale != 0.0 ? (eng - tag.offset) / tag.scale : eng;

    // 写请求文本按标签类型编码
    std::string text;
    if (tag.type == TagDataType::Bool)
        text = raw != 0.0 ? "1" : "0";
    else if (tag.type == TagDataType::Float32) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.9g", raw);
        text = buf;
    } else
        text = std::to_string((int64_t)std::llround(raw));

    std::string resp;
    if (!transactLine(encodeWriteReq(tag.address, tag.type, text), resp, err)) return false;
    WriteAck ack = parseWriteAck(resp);
    if (!ack.ok) {
        err = ack.error.empty() ? ("写应答非法: " + resp) : ack.error;
        return false;
    }
    return true;
}

} // namespace pv::tcp
