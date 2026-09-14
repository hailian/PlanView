// M5 单测：PlanView TCP 协议编解码 / 值文本化 / TCP 数据源回环（含分段重组）
#include "PvTest.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <mutex>
#include <thread>

#include "base/data/DataSourceManager.h"
#include "base/data/tcp/PvProtocol.h"
#include "base/data/tcp/TcpDataSource.h"

using namespace pv;
using namespace pv::tcp;

// ---- 协议编解码 ----
TEST_CASE("协议请求构造") {
    CHECK(encodePing() == "PING\n");
    CHECK(encodeReadReq({100, 3, 7}) == "READ 3 100 3 7\n");
    CHECK(encodeReadReq({}) == "READ 0\n");
    CHECK(encodeWriteReq(100, TagDataType::Float32, "3.5") == "WRITE 100 float32 3.5\n");
}

TEST_CASE("VALUES 行解析") {
    std::vector<ReadItem> items;
    CHECK(parseValuesLine("VALUES 3 100:uint16:1234 3:bool:1 7:float32:3.5", items));
    REQUIRE(items.size() == 3);
    CHECK(items[0].index == 100);
    CHECK(items[0].ok);
    CHECK(std::get<int64_t>(items[0].value) == 1234);
    CHECK(items[1].index == 3);
    CHECK(std::get<bool>(items[1].value) == true);
    CHECK(items[2].ok);
    CHECK(std::get<double>(items[2].value) == 3.5);

    // 无此槽位
    CHECK(parseValuesLine("VALUES 1 9:?:-", items));
    REQUIRE(items.size() == 1);
    CHECK(!items[0].ok);

    // 非法行
    CHECK(!parseValuesLine("VALUES x", items));
    CHECK(!parseValuesLine("VALUES 2 1:uint16:1", items));  // 数量不符
    CHECK(!parseValuesLine("PONG", items));
    CHECK(!parseValuesLine("VALUES 1 1:badtype:5", items));
}

TEST_CASE("写应答解析与值文本化") {
    WriteAck ok = parseWriteAck("OK 100");
    CHECK(ok.ok);
    CHECK(ok.index == 100);
    WriteAck err = parseWriteAck("ERR range");
    CHECK(!err.ok);
    CHECK(err.error == "range");

    CHECK(tagValueToText(true) == "1");
    CHECK(tagValueToText(false) == "0");
    CHECK(tagValueToText((int64_t)42) == "42");
    CHECK(tagValueToText(3.5) == "3.5");
}

// ---- TCP 数据源回环 ----
namespace {

// 极简 PlanView TCP 数据服务器：槽位表 + 行协议；支持把应答按小片段发送（测重组）
class FakeServer {
public:
    void start(int fragmentBytes = 0) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int nodelay = 1;
        setsockopt(listenSock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(listenSock_, (sockaddr*)&addr, sizeof(addr));
        listen(listenSock_, 1);
        sockaddr_in bound = {};
        int len = sizeof(bound);
        getsockname(listenSock_, (sockaddr*)&bound, &len);
        port_ = ntohs(bound.sin_port);
        fragment_ = fragmentBytes;

        running_ = true;
        thread_ = std::thread([this] { serve(); });
    }

    void stop() {
        running_ = false;
        closesocket(listenSock_);
        if (client_ != INVALID_SOCKET) closesocket(client_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }

    int port() const { return port_; }
    void setSlot(int idx, TagDataType t, TagValue v) {
        std::lock_guard<std::mutex> g(m_);
        slots_[idx] = {t, v};
    }
    TagValue slotValue(int idx) {
        std::lock_guard<std::mutex> g(m_);
        auto it = slots_.find(idx);
        return it == slots_.end() ? TagValue{} : it->second.value;
    }

private:
    struct Slot {
        TagDataType type;
        TagValue value;
    };

    void sendAll(SOCKET s, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            size_t chunk = fragment_ > 0 ? std::min((size_t)fragment_, data.size() - sent)
                                         : data.size() - sent;
            int n = ::send(s, data.data() + sent, (int)chunk, 0);
            if (n <= 0) return;
            sent += (size_t)n;
        }
    }

    void serve() {
        while (running_) {
            client_ = accept(listenSock_, nullptr, nullptr);
            if (client_ == INVALID_SOCKET) break;
            handleClient(client_);
        }
    }

    void handleClient(SOCKET s) {
        std::string pending;
        char buf[512];
        while (running_) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            pending.append(buf, (size_t)n);
            // 行拆分（累积缓冲，天然处理 TCP 分段）
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                sendAll(s, handleLine(line));
            }
        }
        closesocket(s);
        client_ = INVALID_SOCKET;
    }

    std::string handleLine(const std::string& line) {
        std::lock_guard<std::mutex> g(m_);
        if (line == "PING") return "PONG\n";
        if (line.rfind("READ ", 0) == 0) {
            std::string out = "VALUES";
            int count = 0;
            size_t pos = 5;
            bool first = true;  // 第一个词是数量，跳过
            while (pos < line.size()) {
                size_t sp = line.find(' ', pos);
                std::string tok = line.substr(pos, sp == std::string::npos ? sp : sp - pos);
                pos = sp == std::string::npos ? line.size() : sp + 1;
                if (tok.empty()) continue;
                if (first) {
                    first = false;
                    continue;
                }
                int idx = std::atoi(tok.c_str());
                auto it = slots_.find(idx);
                if (it != slots_.end()) {
                    TagValue v = it->second.value;
                    if (auto* b = std::get_if<bool>(&v))
                        out += " " + tok + ":" + tagDataTypeName(it->second.type) + ":" + (*b ? "1" : "0");
                    else if (auto* i = std::get_if<int64_t>(&v))
                        out += " " + tok + ":" + tagDataTypeName(it->second.type) + ":" + std::to_string(*i);
                    else if (auto* d = std::get_if<double>(&v)) {
                        char b[32];
                        snprintf(b, sizeof(b), "%.9g", *d);
                        out += " " + tok + ":" + tagDataTypeName(it->second.type) + ":" + b;
                    } else
                        out += " " + tok + ":?:-";
                } else {
                    out += " " + tok + ":?:-";  // 无此槽位
                }
                ++count;
            }
            out = "VALUES " + std::to_string(count) + out.substr(6);
            return out + "\n";
        }
        if (line.rfind("WRITE ", 0) == 0) {
            // WRITE idx type value
            int sp1 = (int)line.find(' ', 6);
            int sp2 = (int)line.find(' ', sp1 + 1);
            int idx = std::atoi(line.substr(6, sp1 - 6).c_str());
            std::string type = line.substr(sp1 + 1, sp2 - sp1 - 1);
            std::string val = line.substr(sp2 + 1);
            auto t = tagDataTypeFromName(type);
            if (!t) return "ERR bad type\n";
            TagValue v;
            if (*t == TagDataType::Bool) v = val == "1" || val == "true";
            else if (*t == TagDataType::Float32) v = std::atof(val.c_str());
            else v = (int64_t)std::atoll(val.c_str());
            slots_[idx] = {*t, v};
            writes_.push_back(line);
            return "OK " + std::to_string(idx) + "\n";
        }
        return "ERR unknown command\n";
    }

    SOCKET listenSock_ = INVALID_SOCKET;
    SOCKET client_ = INVALID_SOCKET;
    int port_ = 0;
    int fragment_ = 0;
    volatile bool running_ = false;
    std::thread thread_;
    std::mutex m_;
    std::map<int, Slot> slots_;

public:
    std::vector<std::string> writes_;
};

} // namespace

void runLoopbackTest(int fragmentBytes) {
    FakeServer server;
    server.setSlot(100, TagDataType::UInt16, TagValue((int64_t)1234));
    server.setSlot(101, TagDataType::Float32, TagValue(2.5));
    server.setSlot(3, TagDataType::Bool, TagValue(false));
    server.start(fragmentBytes);

    TcpSettings st;
    st.host = "127.0.0.1";
    st.port = server.port();

    DataSourceManager mgr;
    auto ds = mgr.createTcp(st);
    std::string err;
    CHECK(ds->connect(err));

    Tag u16;
    u16.name = "u16";
    u16.address = 100;
    u16.type = TagDataType::UInt16;
    u16.scale = 0.01;  // 12.34
    Tag f32;
    f32.name = "f32";
    f32.address = 101;
    f32.type = TagDataType::Float32;
    Tag bit;
    bit.name = "bit";
    bit.address = 3;
    bit.type = TagDataType::Bool;
    Tag missing;
    missing.name = "missing";
    missing.address = 999;
    missing.type = TagDataType::UInt16;

    auto results = ds->readTags({&u16, &f32, &bit, &missing});
    REQUIRE(results.size() == 4);
    CHECK(results[0].ok);
    CHECK(results[1].ok);
    CHECK(results[2].ok);
    CHECK(!results[3].ok);  // 服务器无此槽位
    if (results[0].ok && results[1].ok && results[2].ok) {
        CHECK(std::get<double>(results[0].value) == 12.34);
        CHECK(std::get<double>(results[1].value) == 2.5);
        CHECK(std::get<bool>(results[2].value) == false);
    }

    // 写: float 3.5 写槽位 101, bool true 写槽位 3
    CHECK(ds->writeTag(f32, 3.5, err));
    CHECK(ds->writeTag(bit, true, err));
    CHECK(std::get<double>(server.slotValue(101)) == 3.5);
    CHECK(std::get<bool>(server.slotValue(3)) == true);

    // 写错误值（字符串）-> 失败
    CHECK(!ds->writeTag(u16, std::string("abc"), err));

    ds->disconnect();
    CHECK(!ds->isConnected());
    auto results2 = ds->readTags({&u16});
    CHECK(!results2[0].ok);
    CHECK(results2[0].quality == TagQuality::CommLost);

    server.stop();
}

TEST_CASE("TCP 数据源回环") { runLoopbackTest(0); }

TEST_CASE("TCP 数据源回环(应答分片)") { runLoopbackTest(2); }

TEST_CASE("连接失败") {
    DataSourceManager mgr;
    TcpSettings st;
    st.host = "127.0.0.1";
    st.port = 1;  // 无监听
    auto ds = mgr.createTcp(st);
    static_cast<tcp::TcpDataSource*>(ds.get())->connectTimeoutMs = 300;
    std::string err;
    CHECK(!ds->connect(err));
    CHECK(!err.empty());
}
