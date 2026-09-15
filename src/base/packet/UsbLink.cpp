#include "base/packet/UsbLink.h"

#include "base/log/Log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace pv::packet {

namespace {

// libusb 错误码（本仓不引 SDK 头，按稳定 ABI 数值声明）
constexpr int kErrTimeout = -7; // LIBUSB_ERROR_TIMEOUT

constexpr int kRecvTimeoutMs = 200; // 收包超时：周期返回以检查停止标志
constexpr int kSendTimeoutMs = 1000;
constexpr size_t kInboxCap = 4096; // 消费停摆时丢最旧（尽力而为，与 PcapLink 同策略）
constexpr size_t kRecvBuf = 4096;  // 单次传输缓冲（字节流重组交给 FrameSplitter）
constexpr int kStrBuf = 256;       // 字符串描述符缓冲（与 UsbApi 内一致）

bool parseHex16(const std::string& s, uint16_t& out) {
    if (s.empty() || s.size() > 4) return false;
    unsigned v = 0;
    for (char c : s) {
        v <<= 4;
        char u = (char)std::toupper((unsigned char)c);
        if (u >= '0' && u <= '9') v |= (unsigned)(u - '0');
        else if (u >= 'A' && u <= 'F') v |= (unsigned)(u - 'A' + 10);
        else return false;
    }
    out = (uint16_t)v;
    return true;
}

} // namespace

bool parseDeviceToken(const std::string& token, uint16_t& vid, uint16_t& pid,
                      std::string& serial, std::string& err) {
    serial.clear();
    if (token.empty()) {
        err = "USB 设备为空（应形如 0483:5740 或 0483:5740:SN123）";
        return false;
    }
    size_t c1 = token.find(':');
    if (c1 == std::string::npos) {
        err = "USB 设备格式非法（应形如 0483:5740）: " + token;
        return false;
    }
    size_t c2 = token.find(':', c1 + 1);
    std::string vidS = token.substr(0, c1);
    std::string pidS =
        token.substr(c1 + 1, (c2 == std::string::npos ? token.size() : c2) - c1 - 1);
    if (c2 != std::string::npos) serial = token.substr(c2 + 1);
    if (!parseHex16(vidS, vid) || !parseHex16(pidS, pid)) {
        err = "USB 设备格式非法（vid:pid 需为 1-4 位十六进制）: " + token;
        return false;
    }
    if (c2 != std::string::npos && serial.empty()) {
        err = "USB 设备格式非法（序列号段为空）: " + token;
        return false;
    }
    return true;
}

std::string makeDeviceToken(uint16_t vid, uint16_t pid, const std::string& serial) {
    char buf[96];
    if (serial.empty())
        std::snprintf(buf, sizeof(buf), "%04X:%04X", vid, pid);
    else
        std::snprintf(buf, sizeof(buf), "%04X:%04X:%s", vid, pid, serial.c_str());
    return buf;
}

bool parseEndpointHex(const std::string& text, uint8_t& ep, std::string& err) {
    std::string s = text;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    auto bad = [&]() {
        err = "端点格式非法（应形如 81 / 01）: " + text;
        return false;
    };
    if (s.empty() || s.size() > 2) return bad();
    unsigned v = 0;
    for (char c : s) {
        v <<= 4;
        char u = (char)std::toupper((unsigned char)c);
        if (u >= '0' && u <= '9') v |= (unsigned)(u - '0');
        else if (u >= 'A' && u <= 'F') v |= (unsigned)(u - 'A' + 10);
        else return bad();
    }
    ep = (uint8_t)v;
    return true;
}

bool UsbLink::start(const std::string& device, int interfaceNum, const std::string& epIn,
                    const std::string& epOut, bool requireIn, bool requireOut,
                    std::string& err) {
    stop();

    // ---- 配置校验先于驱动检查（与 PcapLink 一致：无 DLL 机器上仍得到确定性错误）----
    uint16_t vid = 0, pid = 0;
    std::string serial;
    if (!parseDeviceToken(device, vid, pid, serial, err)) return false;
    uint8_t epInOverride = 0, epOutOverride = 0;
    bool hasInOverride = !epIn.empty(), hasOutOverride = !epOut.empty();
    if (hasInOverride && !parseEndpointHex(epIn, epInOverride, err)) return false;
    if (hasOutOverride && !parseEndpointHex(epOut, epOutOverride, err)) return false;

    api_ = libusb::instance(err);
    if (!api_) return false;

    // ---- 枚举匹配设备（vid/pid，配置了 serial 则逐台打开精确比对）----
    libusb::Device** list = nullptr;
    int n = (int)api_->get_device_list(nullptr, &list);
    if (n < 0 || !list) {
        err = std::string("枚举 USB 设备失败: ") + api_->error_name(n);
        return false;
    }
    libusb::Device* found = nullptr;
    for (int i = 0; i < n && !found; ++i) {
        libusb::DeviceDescriptor d{};
        if (api_->get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor != vid || d.idProduct != pid) continue;
        if (!serial.empty()) { // 同型号多台设备靠序列号区分
            libusb::Handle* h = nullptr;
            if (api_->open(list[i], &h) != 0) continue;
            std::string sn;
            if (d.iSerialNumber) {
                unsigned char buf[kStrBuf];
                int len = api_->get_string_ascii(h, d.iSerialNumber, buf, sizeof(buf) - 1);
                if (len > 0) {
                    buf[len] = 0;
                    sn = (const char*)buf;
                }
            }
            api_->close(h);
            if (sn != serial) continue;
        }
        found = list[i];
    }
    if (!found) {
        api_->free_device_list(list, 1);
        err = "未找到 USB 设备 " + device + "（未插入或驱动不兼容）";
        return false;
    }

    // ---- 打开 + 认领接口（open 后设备引用由 handle 持有，列表可释放）----
    if (api_->open(found, (libusb::Handle**)&handle_) != 0) {
        api_->free_device_list(list, 1);
        handle_ = nullptr;
        err = "打开 USB 设备失败 " + device + "（驱动不兼容或被其他程序占用）";
        return false;
    }
    iface_ = interfaceNum;
    int rc = api_->claim_interface((libusb::Handle*)handle_, iface_);
    if (rc != 0) {
        err = std::string("占用 USB 接口 ") + std::to_string(iface_) +
              " 失败（不存在或被占用）: " + api_->error_name(rc);
        api_->close((libusb::Handle*)handle_);
        handle_ = nullptr;
        api_->free_device_list(list, 1);
        return false;
    }

    // ---- 解析端点：override 须存在于接口端点表；自动 = 首个批量、次选中断 ----
    // （描述符查询须在 free_device_list 之前：device 引用尚有效）
    epIn_ = epOut_ = 0;
    epInInterrupt_ = epOutInterrupt_ = false;
    libusb::ConfigDescriptor* cfg = nullptr;
    rc = api_->get_active_config(found, &cfg);
    if (rc != 0 || !cfg) {
        err = std::string("读取 USB 配置描述符失败: ") + api_->error_name(rc);
        api_->release_interface((libusb::Handle*)handle_, iface_);
        api_->close((libusb::Handle*)handle_);
        handle_ = nullptr;
        api_->free_device_list(list, 1);
        return false;
    }
    const libusb::InterfaceDescriptor* ifd = nullptr;
    for (int i = 0; i < cfg->bNumInterfaces && !ifd; ++i)
        for (int a = 0; a < cfg->interfaces[i].num_altsetting; ++a)
            if (cfg->interfaces[i].altsetting[a].bInterfaceNumber == (uint8_t)iface_ &&
                cfg->interfaces[i].altsetting[a].bAlternateSetting == 0) {
                ifd = &cfg->interfaces[i].altsetting[a];
                break;
            }
    if (ifd) {
        uint8_t autoInBulk = 0, autoInIntr = 0, autoOutBulk = 0, autoOutIntr = 0;
        bool inOverrideFound = false, outOverrideFound = false;
        for (int e = 0; e < ifd->bNumEndpoints; ++e) {
            const libusb::EndpointDescriptor& ed = ifd->endpoint[e];
            bool in = (ed.bEndpointAddress & 0x80) != 0;
            bool bulk = (ed.bmAttributes & 3) == 2, intr = (ed.bmAttributes & 3) == 3;
            if (bulk || intr) { // 控制/同步端点不参与（帧收发只用批量/中断）
                if (in) {
                    if (bulk && !autoInBulk) autoInBulk = ed.bEndpointAddress;
                    if (intr && !autoInIntr) autoInIntr = ed.bEndpointAddress;
                } else {
                    if (bulk && !autoOutBulk) autoOutBulk = ed.bEndpointAddress;
                    if (intr && !autoOutIntr) autoOutIntr = ed.bEndpointAddress;
                }
            }
            if (hasInOverride && ed.bEndpointAddress == epInOverride) {
                inOverrideFound = true;
                epIn_ = ed.bEndpointAddress;
                epInInterrupt_ = intr;
            }
            if (hasOutOverride && ed.bEndpointAddress == epOutOverride) {
                outOverrideFound = true;
                epOut_ = ed.bEndpointAddress;
                epOutInterrupt_ = intr;
            }
        }
        if (!hasInOverride) { // 自动：批量优先，中断兜底
            if (autoInBulk) epIn_ = autoInBulk;
            else if (autoInIntr) {
                epIn_ = autoInIntr;
                epInInterrupt_ = true;
            }
        }
        if (!hasOutOverride) {
            if (autoOutBulk) epOut_ = autoOutBulk;
            else if (autoOutIntr) {
                epOut_ = autoOutIntr;
                epOutInterrupt_ = true;
            }
        }
        bool badIn = hasInOverride && !inOverrideFound;
        bool badOut = hasOutOverride && !outOverrideFound;
        if (badIn || badOut) {
            char spec[32];
            std::snprintf(spec, sizeof(spec), "%02X/%02X", epInOverride, epOutOverride);
            err = "USB 接口 " + std::to_string(iface_) + " 无指定端点 (IN/OUT " + spec + ")";
            api_->free_config(cfg);
            api_->release_interface((libusb::Handle*)handle_, iface_);
            api_->close((libusb::Handle*)handle_);
            handle_ = nullptr;
            api_->free_device_list(list, 1);
            return false;
        }
    }
    api_->free_config(cfg);
    api_->free_device_list(list, 1);

    if (requireIn && !epIn_) {
        err = "USB 接口 " + std::to_string(iface_) + " 无可用 IN 端点（数据源需设备上报数据）";
        api_->release_interface((libusb::Handle*)handle_, iface_);
        api_->close((libusb::Handle*)handle_);
        handle_ = nullptr;
        return false;
    }
    if (requireOut && !epOut_) {
        err = "USB 接口 " + std::to_string(iface_) +
              " 无可用 OUT 端点（数据目的需可下发）";
        api_->release_interface((libusb::Handle*)handle_, iface_);
        api_->close((libusb::Handle*)handle_);
        handle_ = nullptr;
        return false;
    }

    running_.store(true, std::memory_order_release);
    if (epIn_) // 数据目的（requireOut）可能无 IN 端点：纯发送链路
        thread_ = std::thread([this] { recvLoop(); });
    PV_LOG_INFO("USB 链路启动: %s 接口 %d, IN %02X(%s), OUT %02X(%s)", device.c_str(), iface_,
                epIn_, epInInterrupt_ ? "中断" : "批量", epOut_,
                epOutInterrupt_ ? "中断" : "批量");
    return true;
}

void UsbLink::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join(); // 收包线程 200ms 超时内自行退出
    if (handle_) {
        api_->release_interface((libusb::Handle*)handle_, iface_);
        api_->close((libusb::Handle*)handle_);
        handle_ = nullptr;
    }
    epIn_ = epOut_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    inbox_.clear();
}

void UsbLink::recvLoop() {
    std::vector<uint8_t> buf(kRecvBuf);
    while (running_.load(std::memory_order_acquire)) {
        int got = 0;
        int rc = epInInterrupt_
                     ? api_->interrupt_transfer((libusb::Handle*)handle_, epIn_, buf.data(),
                                                (int)buf.size(), &got, kRecvTimeoutMs)
                     : api_->bulk_transfer((libusb::Handle*)handle_, epIn_, buf.data(),
                                           (int)buf.size(), &got, kRecvTimeoutMs);
        if (got > 0) { // 超时也可能带回部分数据（libusb 语义）：先入队再判错误码
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(TcpChunk{std::vector<uint8_t>(buf.data(), buf.data() + got)});
            while (inbox_.size() > kInboxCap) inbox_.pop_front();
        }
        if (rc == 0 || rc == kErrTimeout) continue; // 超时 = 周期醒来检查停止标志
        // 拔线/设备移除：置错误并退出线程（isRunning 变 false，上层退避重连）
        std::lock_guard<std::mutex> lock(mutex_);
        lastError_ = std::string("USB 读取失败: ") + api_->error_name(rc);
        running_.store(false, std::memory_order_release);
        return;
    }
}

bool UsbLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (!isRunning() || !handle_) {
        err = "USB 设备未打开";
        return false;
    }
    if (!epOut_) {
        err = "USB 设备无 OUT 端点";
        return false;
    }
    int sent = 0;
    int rc = epOutInterrupt_
                 ? api_->interrupt_transfer((libusb::Handle*)handle_, epOut_,
                                            (unsigned char*)data.data(), (int)data.size(),
                                            &sent, kSendTimeoutMs)
                 : api_->bulk_transfer((libusb::Handle*)handle_, epOut_,
                                       (unsigned char*)data.data(), (int)data.size(), &sent,
                                       kSendTimeoutMs);
    if (rc != 0 || sent != (int)data.size()) {
        err = std::string("USB 写入失败: ") + api_->error_name(rc);
        return false;
    }
    return true;
}

void UsbLink::drain(std::deque<TcpChunk>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

std::string UsbLink::lastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace pv::packet
