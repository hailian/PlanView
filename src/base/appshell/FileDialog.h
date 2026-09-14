// Win32 文件对话框封装（IFileOpenDialog / IFileSaveDialog），UTF-8 路径进出。
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace pv::dialog {

// 过滤器: {显示名, 通配} 例: {"PlanView 工程 (*.json)", "*.json"}
using Filter = std::pair<std::string, std::string>;

// 成功返回 true 且 outPath 为用户选择的 UTF-8 路径；取消/失败返回 false
bool openFile(const std::string& title, const std::vector<Filter>& filters, std::string& outPath);
bool saveFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& defaultName, std::string& outPath);

} // namespace pv::dialog
