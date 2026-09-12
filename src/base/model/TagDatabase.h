// 标签库：增删改查 + 唯一性/地址冲突校验。
#pragma once

#include <string>
#include <vector>

#include "base/model/Tag.h"
#include "base/model/Types.h"

namespace softg {

class TagDatabase {
public:
    std::vector<Tag>& all() { return tags_; }
    const std::vector<Tag>& all() const { return tags_; }

    Tag* find(const TagName& name) {
        for (auto& t : tags_)
            if (t.name == name) return &t;
        return nullptr;
    }
    const Tag* find(const TagName& name) const {
        for (const auto& t : tags_)
            if (t.name == name) return &t;
        return nullptr;
    }

    // 失败返回 false 且 err 写明原因（重名 / 同区同地址同类型冲突）
    bool add(Tag t, std::string& err);
    // 删除标签；调用方（设计器）负责先检查关联引用
    bool remove(const TagName& name);

private:
    std::vector<Tag> tags_;
};

} // namespace softg
