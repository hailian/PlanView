#include "base/data/DataSourceManager.h"

#include "base/data/tcp/TcpDataSource.h"
#include "base/log/Log.h"

#include <winsock2.h>

namespace softg {

DataSourceManager::DataSourceManager() {
    WSADATA wsa;
    wsaOk_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if (!wsaOk_)
        SOFTG_LOG_ERROR("WSAStartup 失败");
}

DataSourceManager::~DataSourceManager() {
    if (wsaOk_) WSACleanup();
}

std::unique_ptr<IDataSource> DataSourceManager::createTcp(const TcpSettings& settings) {
    return std::make_unique<tcp::TcpDataSource>(settings);
}

} // namespace softg
