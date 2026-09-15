// UsbApi — libusb-1.0.dll 运行时动态加载（libusb C API 函数指针表）。
// USB 传输（数据源收帧 / 数据目的转发）经 libusb 访问 WinUSB/libusbK 驱动设备：
// 编译期不依赖 libusb SDK，未放置 DLL 时 instance() 返回 nullptr，上层给放置提示，
// 其余功能不受影响。进程内单例缓存：首次调用探测，结果（含失败）复用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pv::packet::libusb {

// libusb.h 关键类型的本仓镜像（不引 SDK 头；按 libusb-1.0 稳定 ABI 声明）
using Context = void;
using Device = void;
using Handle = void;

// struct libusb_device_descriptor（固定 18 字节布局）
struct DeviceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};

// struct libusb_endpoint_descriptor：bmAttributes 低 2 位 = 传输类型（2=批量 3=中断），
// bEndpointAddress 位 7 = 方向（1=IN 0=OUT）
struct EndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char* extra;
    int extra_length;
};

struct InterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
    const EndpointDescriptor* endpoint;
    const unsigned char* extra;
    int extra_length;
};

struct Interface {
    const InterfaceDescriptor* altsetting;
    int num_altsetting;
    const unsigned char* extra;
    int extra_length;
};

// struct libusb_config_descriptor（成员按 SDK 原名 interface，此处避 windows.h 的
// 同名宏改叫 interfaces——成员名不影响布局）
struct ConfigDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
    const Interface* interfaces;
    const unsigned char* extra;
    int extra_length;
};

// libusb C API 子集（按需声明；任一导出缺失视为 DLL 不可用）。
// 返回值 ssize_t 在 Windows 上与 intptr_t 同宽，按 intptr_t 声明。
struct Api {
    void* module = nullptr; // DLL 句柄（保持常驻）

    int (*init)(Context** ctx) = nullptr;
    intptr_t (*get_device_list)(Context* ctx, Device*** list) = nullptr;
    void (*free_device_list)(Device** list, int unref) = nullptr;
    int (*get_device_descriptor)(Device* dev, DeviceDescriptor* desc) = nullptr;
    int (*open)(Device* dev, Handle** handle) = nullptr;
    void (*close)(Handle* handle) = nullptr;
    int (*get_string_ascii)(Handle* handle, uint8_t index, unsigned char* data,
                            int length) = nullptr;
    int (*get_active_config)(Device* dev, ConfigDescriptor** config) = nullptr;
    void (*free_config)(ConfigDescriptor* config) = nullptr;
    int (*claim_interface)(Handle* handle, int iface) = nullptr;
    int (*release_interface)(Handle* handle, int iface) = nullptr;
    int (*bulk_transfer)(Handle* handle, unsigned char endpoint, unsigned char* data,
                         int length, int* transferred, unsigned int timeout) = nullptr;
    int (*interrupt_transfer)(Handle* handle, unsigned char endpoint, unsigned char* data,
                              int length, int* transferred, unsigned int timeout) = nullptr;
    const char* (*error_name)(int errcode) = nullptr;
};

// 进程内单例：成功返回指针；失败返回 nullptr 并置 err（含 DLL 放置/驱动安装提示）。
// 结果缓存（含失败），不再反复 LoadLibrary
const Api* instance(std::string& err);

// USB 设备信息（UI 下拉显示 + 设备 token 选择）
struct DeviceInfo {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string serial;  // 序列号（设备未提供则空）
    std::string product; // 产品名（字符串描述符读不到则空）
};

// 枚举本机可经 libusb 打开的 USB 设备。驱动不兼容的设备（键鼠/HID 等）open 失败
// 即用不了，不列出——下拉里只剩真正可选的设备。未放置 DLL 时返回空 + err。
std::vector<DeviceInfo> listDevices(std::string& err);

} // namespace pv::packet::libusb
