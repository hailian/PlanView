#include "base/render/TextureCache.h"

#include "base/log/Log.h"

#include <wincodec.h>

#include <cstdint>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace pv {

TextureCache::TextureCache(ID3D11Device* device) : device_(device) {}

TextureCache::~TextureCache() {
    for (auto& [path, entry] : cache_)
        if (entry.srv) entry.srv->Release();
}

const TextureCache::Entry* TextureCache::get(const std::string& path) {
    if (path.empty() || failed_.count(path))
        return nullptr;
    auto it = cache_.find(path);
    if (it != cache_.end())
        return &it->second;

    // 相对路径基于工程目录；绝对路径原样使用
    std::string full = path;
    if (!path.empty() && path[0] != '/' && path[0] != '\\' &&
        path.find(':') == std::string::npos && !baseDir_.empty())
        full = baseDir_ + "/" + path;

    Entry entry;
    if (!load(full, entry)) {
        failed_.insert(path);  // 负缓存：避免每帧重试
        return nullptr;
    }
    auto [inserted, ok] = cache_.emplace(path, entry);
    return &inserted->second;
}

bool TextureCache::load(const std::string& fullPath, Entry& out) {
    // WIC 工厂（进程单例）
    static IWICImagingFactory* wicFactory = [] {
        IWICImagingFactory* f = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&f));
        return SUCCEEDED(hr) ? f : nullptr;
    }();
    if (!wicFactory) {
        PV_LOG_ERROR("WIC 工厂创建失败");
        return false;
    }

    // UTF-8 -> UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, nullptr, 0);
    std::wstring wide(wlen > 0 ? wlen : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fullPath.c_str(), -1, wide.data(), (int)wide.size());

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(wicFactory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &decoder))) {
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;
    do {
        if (FAILED(decoder->GetFrame(0, &frame)) || !frame) break;

        if (FAILED(wicFactory->CreateFormatConverter(&converter)) || !converter) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;

        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w == 0 || h == 0 || w > 16384 || h > 16384) break;

        std::vector<uint8_t> pixels((size_t)w * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data())))
            break;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initData = {pixels.data(), (UINT)(w * 4), 0};

        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device_->CreateTexture2D(&td, &initData, &tex)) || !tex) break;

        D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
        svd.Format = td.Format;
        svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        HRESULT hr = device_->CreateShaderResourceView(tex, &svd, &out.srv);
        tex->Release();
        if (FAILED(hr)) {
            out.srv = nullptr;
            break;
        }
        out.w = (int)w;
        out.h = (int)h;
        ok = true;
    } while (false);

    if (converter) converter->Release();
    if (frame) frame->Release();
    decoder->Release();
    if (ok)
        PV_LOG_INFO("贴图加载成功: %s (%dx%d)", fullPath.c_str(), out.w, out.h);
    return ok;
}

} // namespace pv
