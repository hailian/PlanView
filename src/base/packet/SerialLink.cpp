#include "base/packet/SerialLink.h"

#include "base/log/Log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace pv::packet {

namespace {

// 规范化串口名：COM10+ 必须以 \\.\ 前缀打开；接受 "COM3"/"com3"/"3" 写法
std::string canonicalPortName(const std::string& port) {
    std::string p;
    p.reserve(port.size());
    for (char c : port)
        p.push_back((char)std::toupper((unsigned char)c));
    if (p.empty()) return p;
    if (p.rfind("\\\\.", 0) == 0) return p; // 已带前缀
    if (p.rfind("COM", 0) == 0) return "\\\\.\\" + p;
    return "\\\\.\\COM" + p; // 只写数字："3" -> "\\.\COM3"
}

} // namespace

bool SerialLink::open(const std::string& port, int baud, int dataBits, const std::string& parity,
                      int stopBits, std::string& err) {
    close();

    std::string name = canonicalPortName(port);
    if (name.empty()) {
        err = "串口名为空";
        return false;
    }

    // 打开串口：FILE_FLAG_OVERLAPPED 会引入异步语义复杂度，收发均为独占同步句柄
    HANDLE h = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "打开串口失败 (" + std::to_string(::GetLastError()) + "): " + port;
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!::GetCommState(h, &dcb)) {
        err = "读取串口状态失败 (" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(h);
        return false;
    }
    dcb.BaudRate = (DWORD)std::clamp(baud, 300, 921600);
    dcb.ByteSize = (BYTE)std::clamp(dataBits, 5, 8);
    dcb.Parity = parity == "奇" ? ODDPARITY : parity == "偶" ? EVENPARITY : NOPARITY;
    dcb.StopBits = (stopBits >= 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;               // 二进制模式（Win32 必须为 TRUE）
    dcb.fParity = (dcb.Parity != NOPARITY) ? TRUE : FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; // 部分设备需 DTR 有效才上报数据
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!::SetCommState(h, &dcb)) {
        err = "串口参数设置失败 (" + std::to_string(::GetLastError()) + "): " +
              std::to_string(dcb.BaudRate) + "-" + std::to_string(dcb.ByteSize) + "-" +
              (dcb.Parity == ODDPARITY ? "O" : dcb.Parity == EVENPARITY ? "E" : "N") + "-" +
              (dcb.StopBits == TWOSTOPBITS ? "2" : "1");
        ::CloseHandle(h);
        return false;
    }

    // 读超时：间隔 20ms + 总 100ms，让 ReadFile 周期性返回（部分字节也算一块），
    // 收包线程得以检查退出标志；返回 0 字节不视为错误
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 20;
    to.ReadTotalTimeoutMultiplier = 10;
    to.ReadTotalTimeoutConstant = 100;
    to.WriteTotalTimeoutConstant = 500;
    to.WriteTotalTimeoutMultiplier = 10;
    if (!::SetCommTimeouts(h, &to)) {
        err = "串口超时设置失败 (" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(h);
        return false;
    }
    ::PurgeComm(h, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR);

    handle_ = h;
    open_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { recvLoop(); });
    return true;
}

void SerialLink::close() {
    open_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (handle_) {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::PurgeComm((HANDLE)handle_, PURGE_RXABORT | PURGE_TXABORT);
            ::CloseHandle((HANDLE)handle_);
        }
        handle_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    inbox_.clear();
}

void SerialLink::recvLoop() {
    std::vector<uint8_t> buf(4096);
    while (open_.load(std::memory_order_acquire)) {
        DWORD got = 0;
        if (!::ReadFile((HANDLE)handle_, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            int e = ::GetLastError();
            // 设备移除/拔线：置错误并退出线程（isOpen 变 false，上层重连）
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = "串口读取失败 (" + std::to_string(e) + ")";
            open_.store(false, std::memory_order_release);
            return;
        }
        if (got > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(TcpChunk{std::vector<uint8_t>(buf.data(), buf.data() + got)});
            while (inbox_.size() > 1024) // 防御：消费方停摆时不无限堆积
                inbox_.pop_front();
        }
    }
}

bool SerialLink::send(const std::vector<uint8_t>& data, std::string& err) {
    if (!isOpen() || !handle_) {
        err = "串口未打开";
        return false;
    }
    DWORD wrote = 0;
    if (!::WriteFile((HANDLE)handle_, data.data(), (DWORD)data.size(), &wrote, nullptr) ||
        wrote != data.size()) {
        err = "串口写入失败 (" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    return true;
}

void SerialLink::drain(std::deque<TcpChunk>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!inbox_.empty()) {
        out.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
}

std::string SerialLink::lastError() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

} // namespace pv::packet
