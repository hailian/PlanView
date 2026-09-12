// TextureCache — Image 组件的贴图缓存（WIC 解码 -> DX11 SRV，按路径缓存，失败负缓存）。
#pragma once

#include <d3d11.h>

#include <map>
#include <set>
#include <string>

namespace softg {

class TextureCache {
public:
    struct Entry {
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
    };

    // device 归调用方所有（AppShell 的 DX11 设备）
    explicit TextureCache(ID3D11Device* device);
    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // 相对 baseDir 解析路径；成功返回缓存的条目（生命周期归缓存），失败返回 nullptr
    const Entry* get(const std::string& path);

    // 相对路径的基准目录（工程文件所在目录）
    void setBaseDir(const std::string& dir) { baseDir_ = dir; }

private:
    bool load(const std::string& fullPath, Entry& out);

    ID3D11Device* device_ = nullptr;
    std::string baseDir_;
    std::map<std::string, Entry> cache_;
    std::set<std::string> failed_;
};

} // namespace softg
