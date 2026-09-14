#include "planner/Document.h"

namespace pv::planner {

void Document::commit(std::string_view label) {
    undo_.push_back(current_);
    if (undo_.size() > kMaxUndo)
        undo_.pop_front();
    redo_.clear();
    lastLabel_ = std::string(label);
    dirty_ = true;
}

bool Document::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(current_));
    current_ = std::move(undo_.back());
    undo_.pop_back();
    ensureDefaultProject();
    dirty_ = true;
    return true;
}

bool Document::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(current_));
    current_ = std::move(redo_.back());
    redo_.pop_back();
    ensureDefaultProject();
    dirty_ = true;
    return true;
}

void Document::reset(Project p) {
    current_ = std::move(p);
    ensureDefaultProject();
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    lastLabel_.clear();
}

} // namespace pv::planner
