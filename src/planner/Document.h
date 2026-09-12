// Document — 工程文档持有 + 快照式 undo/redo + dirty 标记。
// 约定：所有编辑操作必须"先 commit 再改"（commit 把当前状态压入 undo 栈）。
#pragma once

#include <deque>
#include <string>
#include <string_view>

#include "base/model/Project.h"

namespace softg::planner {

class Document {
public:
    Document() { ensureDefaultProject(); }

    Project& project() { return current_; }
    const Project& project() const { return current_; }

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    const std::string& lastLabel() const { return lastLabel_; }
    const std::string& path() const { return path_; }
    void setPath(const std::string& p) { path_ = p; }

    // 变更前调用：压栈当前快照。label 例: "移动组件"、"删除 3 个组件"
    void commit(std::string_view label);
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // 载入/重置（不产生 undo 记录）
    void reset(Project p);

    static constexpr size_t kMaxUndo = 100;

private:
    void ensureDefaultProject() {
        if (current_.pages.empty()) {
            Page pg;
            pg.id = "page-1";
            pg.name = "页面 1";
            current_.nextId = std::max<uint64_t>(current_.nextId, 2);
            current_.pages.push_back(std::move(pg));
        }
    }

    Project current_;
    std::deque<Project> undo_, redo_;
    std::string lastLabel_;
    std::string path_;
    bool dirty_ = false;
};

} // namespace softg::planner
