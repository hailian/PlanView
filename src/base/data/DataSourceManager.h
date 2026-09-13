// DataSourceManager — WSA 会话 RAII + 数据源工厂。
// 新协议 = 新工厂方法 + IDataSource 实现。
#pragma once

#include <memory>
#include <string>

#include "base/data/IDataSource.h"
#include "base/data/frame/FrameSourceSettings.h"

namespace softg {

class DataSourceManager {
public:
    // 构造时 WSAStartup，析构时 WSACleanup（进程内多处构造安全：引用计数）
    DataSourceManager();
    ~DataSourceManager();

    DataSourceManager(const DataSourceManager&) = delete;
    DataSourceManager& operator=(const DataSourceManager&) = delete;

    std::unique_ptr<IDataSource> createTcp(const TcpSettings& settings);
    std::unique_ptr<IDataSource> createFrame(const FrameSourceSettings& settings);

private:
    bool wsaOk_ = false;
};

} // namespace softg
