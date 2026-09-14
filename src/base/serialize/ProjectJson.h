// 工程配置文件读写：LogicPlanner 写，PageViewer 读。
// 版本策略：schemaVersion 高于当前支持的版本拒载；未知属性键保留透传（round-trip 不丢）。
#pragma once

#include <string>

#include "base/model/Project.h"

namespace pv::projio {

inline constexpr int kCurrentSchemaVersion = 1;

// 保存工程到 path（UTF-8, 缩进 2）。失败返回 false + err。
bool save(const std::string& path, const Project& project, std::string& err);

// 从 path 加载工程。失败返回 false + err（人可读）。
bool load(const std::string& path, Project& project, std::string& err);

// 序列化到字符串（单测 / 最近文件预览用）
std::string dump(const Project& project);
// 从字符串解析
bool parse(const std::string& text, Project& project, std::string& err);

} // namespace pv::projio
