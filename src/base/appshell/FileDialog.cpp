#include "base/appshell/FileDialog.h"

#include <shobjidl.h>
#include <windows.h>

#include <cstdint>

#pragma comment(lib, "ole32.lib")

namespace softg::dialog {

namespace {

// COM 初始化（按线程一次）
struct ComInit {
    bool ok = false;
    ComInit() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComInit() {
        if (ok) CoUninitialize();
    }
};

std::wstring toWide(const std::string& utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(len > 0 ? len : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), (int)w.size());
    return w;
}
std::string toUtf8(const wchar_t* wide) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len : 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

// 通用对话框流程。save 为 true 时用保存对话框。
bool runDialog(const std::string& title, const std::vector<Filter>& filters,
               const std::string& defaultName, bool save, std::string& outPath) {
    ComInit com;
    if (!com.ok) return false;

    IFileDialog* dlg = nullptr;
    CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    IID iid = save ? IID_IFileSaveDialog : IID_IFileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void**)&dlg)) || !dlg)
        return false;

    bool ok = false;
    do {
        dlg->SetTitle(toWide(title).c_str());

        // 过滤器（COM 双 null 结尾数组）
        std::vector<COMDLG_FILTERSPEC> specs;
        std::vector<std::wstring> names, patterns;
        for (const auto& f : filters) {
            names.push_back(toWide(f.first));
            patterns.push_back(toWide(f.second));
        }
        for (size_t i = 0; i < filters.size(); ++i)
            specs.push_back({names[i].c_str(), patterns[i].c_str()});
        if (!specs.empty() && FAILED(dlg->SetFileTypes((UINT)specs.size(), specs.data())))
            break;
        if (!specs.empty()) dlg->SetDefaultExtension(toWide(filters[0].second).c_str());
        if (save && !defaultName.empty())
            dlg->SetFileName(toWide(defaultName).c_str());

        DWORD options = 0;
        dlg->GetOptions(&options);
        if (!save) dlg->SetOptions(options | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);

        if (FAILED(dlg->Show(nullptr))) break;  // 用户取消也走这里

        IShellItem* item = nullptr;
        if (FAILED(dlg->GetResult(&item)) || !item) break;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            outPath = toUtf8(path);
            CoTaskMemFree(path);
            ok = true;
        }
        item->Release();
    } while (false);

    dlg->Release();
    return ok;
}

} // namespace

bool openFile(const std::string& title, const std::vector<Filter>& filters, std::string& outPath) {
    return runDialog(title, filters, std::string(), false, outPath);
}

bool saveFile(const std::string& title, const std::vector<Filter>& filters,
              const std::string& defaultName, std::string& outPath) {
    return runDialog(title, filters, defaultName, true, outPath);
}

} // namespace softg::dialog
