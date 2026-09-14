#include "base/packet/PcapLink.h"

#include "base/log/Log.h"

#include <cstdio>
#include <cstring>

namespace pv::packet {

namespace {

std::string ipToString(const uint8_t* p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

} // namespace

bool parseEthernetFrame(const uint8_t* d, size_t n, CapPacket& out) {
    if (n < 14) return false;
    uint16_t et = (uint16_t)((d[12] << 8) | d[13]);
    size_t off = 14;
    // VLAN 标签栈（802.1Q 单层 / 802.1ad QinQ 双层）：镜像口常见，逐层剥到真实 EtherType
    while (et == 0x8100 || et == 0x88A8) {
        if (n < off + 4) return false;
        et = (uint16_t)((d[off + 2] << 8) | d[off + 3]);
        off += 4;
    }
    if (et != 0x0800) return false; // 仅 IPv4（与全仓一致；ARP/IPv6 等不驱动字段）

    const uint8_t* ip = d + off;
    const size_t ipAvail = n - off;
    if (ipAvail < 20) return false;
    if ((ip[0] >> 4) != 4) return false;
    const size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    if (ihl < 20 || ipAvail < ihl) return false;
    // IP 总长是负载边界权威（以太帧尾 padding 不算），caplen 不足则按截断丢弃
    const size_t total = (size_t)((ip[2] << 8) | ip[3]);
    if (total < ihl || ipAvail < total) return false;

    const uint8_t* t = ip + ihl;
    const size_t tLen = total - ihl;
    out = CapPacket{};
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
    if (ip[9] == 6) { // TCP
        if (tLen < 20) return false;
        const size_t doff = (size_t)(t[12] >> 4) * 4;
        if (doff < 20 || tLen < doff) return false;
        out.srcPort = (uint16_t)((t[0] << 8) | t[1]);
        out.dstPort = (uint16_t)((t[2] << 8) | t[3]);
        out.tcp = true;
        payload = t + doff;
        payloadLen = tLen - doff;
    } else if (ip[9] == 17) { // UDP
        if (tLen < 8) return false;
        const size_t ulen = (size_t)((t[4] << 8) | t[5]);
        if (ulen < 8) return false;
        // UDP 长度字段为准，但不超过 IP 负载（padding 场景二者可能不等）
        const size_t udpLen = ulen < tLen ? ulen : tLen;
        out.srcPort = (uint16_t)((t[0] << 8) | t[1]);
        out.dstPort = (uint16_t)((t[2] << 8) | t[3]);
        payload = t + 8;
        payloadLen = udpLen - 8;
    } else {
        return false;
    }
    out.srcIp = ipToString(ip + 12);
    out.dstIp = ipToString(ip + 16);
    out.payload.assign(payload, payload + payloadLen);
    out.flowKey = out.srcIp + ":" + std::to_string(out.srcPort) + ">" + out.dstIp + ":" +
                  std::to_string(out.dstPort);
    return true;
}

std::string buildListenBpf(const std::string& dip, int dport, bool tcp) {
    const char* pr = tcp ? "tcp" : "udp";
    // dip="*"：不限 IP，命中 = 源或目的端口为 dport（任一方向全收）
    std::string core = (dip.empty() || dip == "*")
                           ? std::string(pr) + " port " + std::to_string(dport)
                           : "host " + dip + " and " + pr + " port " + std::to_string(dport);
    // 带 VLAN 标签的镜像帧 EtherType 是 0x8100/0x88A8，普通表达式不命中，
    // 须用 vlan 原语显式并列（vlan 后的表达式隐含已剥标签语义）
    return "(" + core + ") or (vlan and " + core + ")";
}

// ---- FlowSplitters ----

void FlowSplitters::setConfig(const FramingConfig& config) {
    cfg_ = config;
    cfgs_.clear();
    reset();
}

void FlowSplitters::setConfigs(const std::vector<FramingConfig>& configs) {
    cfgs_ = configs;
    if (!configs.empty()) cfg_ = configs.front();
    reset();
}

void FlowSplitters::feed(const CapPacket& pkt, std::vector<std::vector<uint8_t>>& out) {
    if (!pkt.tcp) { // UDP 数据报天然成帧：一包一帧
        out.push_back(pkt.payload);
        return;
    }
    auto it = flows_.find(pkt.flowKey);
    if (it == flows_.end()) {
        if (flows_.size() >= 32) flows_.clear(); // 流数保护：整体重建，靠帧头重同步
        FrameSplitter sp(cfg_);
        if (!cfgs_.empty()) sp.setConfigs(cfgs_);
        it = flows_.emplace(pkt.flowKey, std::move(sp)).first;
    }
    it->second.feed(pkt.payload.data(), pkt.payload.size(), out);
}

// ---- PcapLink ----

bool PcapLink::start(const std::string& device, const std::string& bpf, std::string& err) {
    stop();
    if (device.empty()) { // 配置错误先于驱动检测报告（未选网卡与是否装 Npcap 无关）
        err = "未选择抓包网卡";
        return false;
    }
    const npcap::Api* api = npcap::instance(err);
    if (!api) return false; // 未安装 Npcap 等（err 已含安装提示）

    char ebuf[256];
    // 混杂模式：镜像流量 MAC/IP 均非本机，必须收下所有帧；to_ms=200ms 保证
    // dispatch 周期性醒来检查退出标志
    void* h = api->open_live(device.c_str(), 65535, 1, 200, ebuf);
    if (!h) {
        err = std::string("打开网卡失败: ") + ebuf;
        return false;
    }
    // 链路层须是以太网封装（WiFi/环回在 Npcap 下也呈 EN10MB）
    if (api->datalink(h) != 1 /* DLT_EN10MB */) {
        err = "网卡链路层非以太网封装，暂不支持";
        api->close(h);
        return false;
    }
    npcap::BpfProgram prog{};
    // netmask 取 PCAP_NETMASK_UNKNOWN：以太网卡上点分/host 过滤不受影响
    if (api->compile(h, &prog, bpf.c_str(), 1, 0xFFFFFFFFu) < 0) {
        err = std::string("过滤器编译失败: ") + api->geterr(h);
        api->close(h);
        return false;
    }
    int sf = api->setfilter(h, &prog);
    api->freecode(&prog);
    if (sf < 0) {
        err = std::string("安装过滤器失败: ") + api->geterr(h);
        api->close(h);
        return false;
    }

    api_ = api;
    handle_ = h;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_.clear();
    }
    thread_ = std::thread([this] { capLoop(); });
    PV_LOG_INFO("抓包监听启动: %s (过滤: %s)", device.c_str(), bpf.c_str());
    return true;
}

void PcapLink::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (handle_) {
        api_->close(handle_);
        handle_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.clear(); // 重启不吐旧包
    }
    PV_LOG_INFO("抓包监听停止");
}

void PcapLink::drain(std::deque<CapPacket>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

std::string PcapLink::lastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void PcapLink::onPacket(unsigned char* user, const npcap::PcapPkthdr* hdr,
                        const unsigned char* bytes) {
    PcapLink* self = (PcapLink*)user;
    if (!hdr || hdr->caplen == 0) return;
    CapPacket pkt;
    if (!parseEthernetFrame(bytes, hdr->caplen, pkt)) return;
    if (pkt.payload.empty()) return; // TCP 纯 ACK / 空数据报不驱动字段
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->inbox_.size() >= 4096)
        self->inbox_.pop_front(); // 消费停滞保护：丢最旧（镜像口流量洪峰）
    self->inbox_.push_back(std::move(pkt));
}

void PcapLink::capLoop() {
    while (running_) {
        // count=0：处理当前缓冲内全部命中包；to_ms 超时返回 0 继续轮询退出标志
        int n = api_->dispatch(handle_, 0, &PcapLink::onPacket, (unsigned char*)this);
        if (n < 0) { // 抓包句柄错误（不再重试，交由上层断线重连机制）
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "抓包读取失败: " + std::string(api_->geterr(handle_));
            break;
        }
    }
}

} // namespace pv::packet
