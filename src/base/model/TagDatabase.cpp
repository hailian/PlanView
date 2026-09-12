#include "base/model/TagDatabase.h"

namespace softg {

bool TagDatabase::add(Tag t, std::string& err) {
    if (t.name.empty()) {
        err = "标签名不能为空";
        return false;
    }
    if (find(t.name)) {
        err = "标签名重复: " + t.name;
        return false;
    }
    // 同槽位同类型冲突（同槽位不同类型由用户自行保证语义）
    for (const auto& exist : tags_) {
        if (exist.address == t.address && exist.type == t.type) {
            err = "槽位冲突: " + std::to_string(t.address) + " 已被标签 " + exist.name + " 使用";
            return false;
        }
    }
    tags_.push_back(std::move(t));
    return true;
}

bool TagDatabase::remove(const TagName& name) {
    for (auto it = tags_.begin(); it != tags_.end(); ++it) {
        if (it->name == name) {
            tags_.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace softg
