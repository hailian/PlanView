#include "base/packet/VisaLink.h"

#include "base/log/Log.h"

namespace pv::packet {

namespace {

constexpr visa::ViUInt32 kRecvTimeoutMs = 200; // 收包超时：周期返回以检查停止标志
constexpr visa::ViUInt32 kSendTimeoutMs = 1000; // 纯发送链路（数据目的）的读写超时
constexpr size_t kInboxCap = 4096; // 消费停摆时丢最旧（尽力而为，与 USB 链路同策略）
constexpr size_t kRecvBuf = 4096;  // 单次读缓冲（字节流重组交给 FrameSplitter）

} // namespace

bool validateVisaAddress(const std::string& address, std::string& err) {
    if (address.empty()) {
        err = "VISA 地址为空（应形如 TCPIP0::192.168.1.5::inst0::INSTR）";
        return false;
    }
    if (address.size() >= visa::kFindBufLen) {
        err = "VISA 地址过长（超 " + std::to_string(visa::kFindBufLen - 1) + " 字符）";
        return false;
    }
    return true;
}

bool VisaLink::start(const std::string& address, bool requireRecv, std::string& err) {
    stop();

    // ---- 地址校验先于运行时检查（与 PcapLink/UsbLink 同策略）----
    if (!validateVisaAddress(address, err)) return false;

    api_ = visa::instance(err);
    if (!api_) return false;

    // 打开仪器：默认 RM 每链路一个（viOpenDefaultRM 引用计数，代价低），
    // viOpen mode=0 不加锁（同机多进程可共享仪器）
    visa::ViStatus st = api_->openDefaultRM(&rm_);
    if (st < 0) {
        err = "VISA 初始化失败: " + visa::statusText(api_, 0, st);
        rm_ = 0;
        return false;
    }
    st = api_->open(rm_, address.c_str(), 0, 0, &vi_);
    if (st < 0) {
        err = "打开 VISA 仪器失败（" + address + "）: " + visa::statusText(api_, rm_, st);
        api_->close(rm_);
        rm_ = 0;
        return false;
    }
    // 读超时：收包链路 200ms（线程得以周期检查停止标志）；纯发送链路放宽到 1s
    //（VI_ATTR_TMO_VALUE 读写共用，收发并存时取收包侧小值）
    api_->setAttribute(vi_, visa::kAttrTmoValue,
                       requireRecv ? kRecvTimeoutMs : kSendTimeoutMs);

    running_.store(true, std::memory_order_release);
    if (requireRecv)
        thread_ = std::thread([this] { recvLoop(); });
    PV_LOG_INFO("VISA 链路启动: %s（收包 %s）", address.c_str(), requireRecv ? "开" : "关");
    return true;
}

void VisaLink::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join(); // 收包线程于读超时内自行退出
    if (vi_) {
        api_->close(vi_);
        vi_ = 0;
    }
    if (rm_) {
        api_->close(rm_);
        rm_ = 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    inbox_.clear();
}

void VisaLink::recvLoop() {
    std::vector<uint8_t> buf(kRecvBuf);
    while (running_.load(std::memory_order_acquire)) {
        visa::ViUInt32 got = 0;
        visa::ViStatus st = api_->read(vi_, buf.data(), (visa::ViUInt32)buf.size(), &got);
        if (got > 0) { // 部分实现超时也可能带回数据：先入队再判状态码
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(TcpChunk{std::vector<uint8_t>(buf.data(), buf.data() + got)});
            while (inbox_.size() > kInboxCap) inbox_.pop_front();
        }
        if (st >= 0) continue;               // 成功或警告（如缓冲不足截断）
        if (st == visa::kErrorTmo) continue; // 超时 = 周期醒来检查停止标志
        // 拔线/设备移除：置错误并退出线程（isRunning 变 false，上层退避重连）
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = std::string("VISA 读取失败: ") + visa::statusText(api_, vi_, st);
        running_.store(false, std::memory_order_release);
        return;
    }
}

bool VisaLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (!isRunning() || !vi_) {
        err = "VISA 仪器未打开";
        return false;
    }
    visa::ViUInt32 sent = 0;
    visa::ViStatus st = api_->write(vi_, data.data(), (visa::ViUInt32)data.size(), &sent);
    if (st < 0 || sent != data.size()) {
        err = std::string("VISA 写入失败: ") + visa::statusText(api_, vi_, st);
        return false;
    }
    return true;
}

void VisaLink::drain(std::deque<TcpChunk>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

std::string VisaLink::lastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace pv::packet
