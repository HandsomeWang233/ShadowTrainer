#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>

#include "webview_host.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include <WebView2.h>

#include "resource_ids.h"

namespace ce::web {
namespace {

constexpr wchar_t virtual_host[] = L"https://shadowtrainer.local/";
// The filter is a URI pattern, not a prefix: without the wildcard only the exact
// URL matches and every real request escapes to the network.
constexpr wchar_t virtual_host_pattern[] = L"https://shadowtrainer.local/*";
constexpr wchar_t start_url[] = L"https://shadowtrainer.local/index.html";
// The page's own base colour, split so the controller can be told about it
// without the BYTE-truncating RGB macros.
constexpr BYTE background_r = 0x0c;
constexpr BYTE background_g = 0x0d;
constexpr BYTE background_b = 0x0f;

// A refcounted COM holder. WRL is not installed with this toolset, so the few
// smart-pointer duties this file needs are done here rather than pulling in ATL.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    // Owning construction: the caller hands over its reference. Matches every
    // Create*/get_* result in this file.
    explicit ComPtr(T* owned) noexcept : p_(owned) {}
    ~ComPtr() { reset(); }
    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) { if (other.p_) other.p_->AddRef(); reset(); p_ = other.p_; }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { reset(); p_ = other.p_; other.p_ = nullptr; }
        return *this;
    }
    // Assignment from a raw pointer AddRefs: the caller keeps ownership of what
    // it handed over. A pointer that arrives in a completion callback is
    // borrowed and needs exactly this; without it the object dies when the
    // callback returns.
    ComPtr& operator=(T* borrowed) noexcept {
        if (p_ != borrowed) {
            if (borrowed) borrowed->AddRef();
            reset();
            p_ = borrowed;
        }
        return *this;
    }
    // Takes over a reference the caller already owns, without AddRef.
    void attach(T* owned) noexcept {
        if (p_ != owned) { reset(); p_ = owned; }
    }
    T* get() const noexcept { return p_; }
    // For out-parameters: releases whatever was held and returns the address.
    T** put() noexcept { reset(); return &p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept { if (p_) { p_->Release(); p_ = nullptr; } }
private:
    T* p_ = nullptr;
};

struct Asset {
    // One string for both jobs, so they cannot drift apart: it is the path the
    // page asks the virtual origin for, the path under web/ that the .rc embeds,
    // and the path the --dev disk read opens.
    const wchar_t* path;
    int resource_id;
    const wchar_t* mime;
};

// Every embedded asset, with the MIME type stated rather than sniffed. The path
// is relative to web/ and to the virtual origin at the same time -- the page
// asks for /js/app.js and webui.rc embeds js/app.js -- so this list,
// web/webui.rc and web/resource_ids.h all have to agree. They fail differently:
// a path here that the .rc does not embed is a 404 at runtime, and a path the
// .rc names that is not on disk stops the build.
const Asset assets[] = {
    {L"index.html", IDR_WEB_INDEX, L"text/html; charset=utf-8"},
    {L"css/app.css", IDR_WEB_APP_CSS, L"text/css; charset=utf-8"},
    {L"js/format.js", IDR_WEB_FORMAT_JS, L"text/javascript; charset=utf-8"},
    {L"js/bridge.js", IDR_WEB_BRIDGE_JS, L"text/javascript; charset=utf-8"},
    {L"js/gating.js", IDR_WEB_GATING_JS, L"text/javascript; charset=utf-8"},
    {L"js/chrome.js", IDR_WEB_CHROME_JS, L"text/javascript; charset=utf-8"},
    {L"js/app.js", IDR_WEB_APP_JS, L"text/javascript; charset=utf-8"},
    {L"js/boot.js", IDR_WEB_BOOT_JS, L"text/javascript; charset=utf-8"},
    {L"js/scan.js", IDR_WEB_SCAN_JS, L"text/javascript; charset=utf-8"},
    {L"js/address.js", IDR_WEB_ADDRESS_JS, L"text/javascript; charset=utf-8"},
    {L"js/memory.js", IDR_WEB_MEMORY_JS, L"text/javascript; charset=utf-8"},
    {L"js/hexview.js", IDR_WEB_HEXVIEW_JS, L"text/javascript; charset=utf-8"},
    {L"js/views.js", IDR_WEB_VIEWS_JS, L"text/javascript; charset=utf-8"},
    {L"js/pointer.js", IDR_WEB_POINTER_JS, L"text/javascript; charset=utf-8"},
    {L"js/sort.js", IDR_WEB_SORT_JS, L"text/javascript; charset=utf-8"},
    {L"js/modal.js", IDR_WEB_MODAL_JS, L"text/javascript; charset=utf-8"},
    {L"img/brand.png", IDR_WEB_BRAND_PNG, L"image/png"},
};

std::wstring local_app_data() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer)) return {};
    return buffer;
}

void remove_tree(const std::wstring& path) noexcept;

// A folder left behind by a process that is gone. A live sibling is left alone:
// two injected hosts each need their own profile, and WebView2 refuses to share
// one. OpenProcess failing with access-denied means "probably alive, or at least
// not ours to touch", which is treated as alive.
void remove_stale_folders(const std::wstring& root) {
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const wchar_t* name = entry.cFileName;
        if (name[0] < L'0' || name[0] > L'9') continue;
        const DWORD pid = std::wcstoul(name, nullptr, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process) {
            if (GetLastError() != ERROR_INVALID_PARAMETER) continue;   // not provably dead
        } else {
            const DWORD wait = WaitForSingleObject(process, 0);
            CloseHandle(process);
            if (wait == WAIT_TIMEOUT) continue;                        // still running
        }
        remove_tree(root + L"\\" + name);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

// The data folder must never be the host executable's own directory: this DLL is
// injected into an arbitrary process, and that directory is frequently not
// writable (and must not be written to even when it is). A per-pid folder also
// keeps two injected hosts from fighting over one WebView2 profile.
std::wstring user_data_folder() {
    const std::wstring root = local_app_data();
    if (root.empty()) return {};
    const std::wstring parent = root + L"\\ShadowTrainer\\WebView2";
    const std::wstring folder = parent + L"\\" + std::to_wstring(GetCurrentProcessId());
    remove_stale_folders(parent);
    // CreateDirectory only makes the last component, so walk the path.
    std::wstring partial;
    for (size_t i = 0; i < folder.size(); ++i) {
        partial += folder[i];
        if (folder[i] == L'\\' && partial.size() > 3) CreateDirectoryW(partial.c_str(), nullptr);
    }
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

void remove_tree(const std::wstring& path) noexcept {
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = path + L"\\*";
    HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(entry.cFileName, L".") == 0 || std::wcscmp(entry.cFileName, L"..") == 0)
                continue;
            const std::wstring child = path + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child.c_str());
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}

// SHADOWTRAINER_WEBUI_DIR points at web/ during development; unset means "serve
// the copies compiled into the DLL".
bool development_root(std::wstring& root) {
    wchar_t buffer[1024]{};
    const DWORD length = GetEnvironmentVariableW(L"SHADOWTRAINER_WEBUI_DIR", buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer)) return false;
    root.assign(buffer, length);
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    return !root.empty();
}

bool development_tools() {
    wchar_t buffer[16]{};
    const DWORD length = GetEnvironmentVariableW(L"SHADOWTRAINER_WEBUI_DEV", buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    return length && buffer[0] == L'1';
}

// A development path comes out of the asset table below, never out of a request,
// so it is allowed to name a subdirectory -- but a ".." segment, a drive letter
// or a rooted path still has no business being in there.
bool safe_relative_path(const std::wstring& path) {
    if (path.empty() || path.size() > 128) return false;
    if (path.find(L"..") != std::wstring::npos) return false;
    if (path.find(L':') != std::wstring::npos) return false;
    return path.front() != L'\\' && path.front() != L'/';
}

bool read_file(const std::wstring& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (16 << 20)) {
        CloseHandle(file);
        return false;
    }
    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                              &read, nullptr);
    CloseHandle(file);
    if (!ok || read != bytes.size()) {
        bytes.clear();
        return false;
    }
    return true;
}

bool load_asset(HINSTANCE module, const Asset& asset, std::string& bytes) {
    std::wstring root;
    if (development_root(root) && safe_relative_path(asset.path)) {
        if (read_file(root + L"\\" + asset.path, bytes)) return true;
    }
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(asset.resource_id), RT_RCDATA);
    if (!found) return false;
    const DWORD size = SizeofResource(module, found);
    const HGLOBAL loaded = LoadResource(module, found);
    if (!loaded) return false;
    const void* data = LockResource(loaded);
    if (!data || !size) return false;
    bytes.assign(static_cast<const char*>(data), size);
    return true;
}

// Exact equality against the table is the whole guard on the request path: a
// hand-written URL can only ever name one of the seventeen entries above. That
// still holds now that the entries carry separators, because the match is
// against a fixed list rather than against the filesystem -- no request-derived
// string is ever handed to CreateFileW. Adding a pattern match or a prefix
// match here is what would break that, not the separators.
const Asset* find_asset(const std::wstring& path) {
    for (const Asset& asset : assets)
        if (path == asset.path) return &asset;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------------

struct Host::Impl {
    enum class State { idle, creating_environment, creating_controller, ready, failed };

    HWND window = nullptr;
    HINSTANCE module = nullptr;
    MessageHandler handler;

    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    ComPtr<ICoreWebView2Settings> settings;

    EventRegistrationToken message_token{};
    EventRegistrationToken resource_token{};
    EventRegistrationToken failed_token{};
    bool message_registered = false;
    bool resource_registered = false;
    bool failed_registered = false;

    std::atomic<State> state{State::idle};
    // When set, the controller is created but left hidden until reveal(), so
    // the window's own startup cover owns the screen instead of the page.
    bool hold_visible = false;
    bool revealed = false;
    // Set before the controller is closed, so nothing in this process calls into
    // a half-torn-down WebView while Close() pumps its own messages.
    std::atomic<bool> closing{false};
    std::wstring error;
    RECT bounds{};
    bool bounds_valid = false;

    void fail(HRESULT hr, const wchar_t* what) noexcept {
        wchar_t text[512]{};
        std::swprintf(text, std::size(text), L"%s (HRESULT 0x%08lX)", what,
                      static_cast<unsigned long>(hr));
        error = text;
        state.store(State::failed);
    }

    void on_environment_ready(HRESULT result, ICoreWebView2Environment* created) noexcept;
    void on_controller_ready(HRESULT result, ICoreWebView2Controller* created) noexcept;
    void on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept;
    void on_resource_requested(ICoreWebView2WebResourceRequestedEventArgs* args) noexcept;
    void apply_settings() noexcept;
    void serve(ICoreWebView2WebResourceRequestedEventArgs* args, const std::wstring& path) noexcept;
};

namespace {

// Every handler below is a single-use COM object with the same boilerplate; none
// of them outlives the session. WebView2 AddRefs the handler it is given, so the
// caller releases its own reference right after registering.
#define CE_WEBVIEW_HANDLER(CLASS, INTERFACE, IID_NAME, BODY)                       \
    class CLASS final : public INTERFACE {                                          \
    public:                                                                         \
        explicit CLASS(Host::Impl* impl) : impl_(impl) {}                           \
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override { \
            if (!object) return E_POINTER;                                           \
            if (id == IID_IUnknown || id == IID_NAME) {                              \
                *object = static_cast<INTERFACE*>(this);                             \
                AddRef();                                                            \
                return S_OK;                                                         \
            }                                                                        \
            *object = nullptr;                                                       \
            return E_NOINTERFACE;                                                     \
        }                                                                            \
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }                \
        ULONG STDMETHODCALLTYPE Release() override {                                 \
            const ULONG left = --refs_;                                              \
            if (!left) delete this;                                                  \
            return left;                                                             \
        }                                                                            \
        BODY                                                                         \
    private:                                                                         \
        std::atomic<ULONG> refs_{1};                                                 \
        Host::Impl* impl_;                                                           \
    };

CE_WEBVIEW_HANDLER(EnvironmentReady, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                   IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                   HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                                    ICoreWebView2Environment* created) override {
                       impl_->on_environment_ready(result, created);
                       return S_OK;
                   })

CE_WEBVIEW_HANDLER(ControllerReady, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                   IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                   HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                                    ICoreWebView2Controller* created) override {
                       impl_->on_controller_ready(result, created);
                       return S_OK;
                   })

CE_WEBVIEW_HANDLER(MessageReceived, ICoreWebView2WebMessageReceivedEventHandler,
                   IID_ICoreWebView2WebMessageReceivedEventHandler,
                   HRESULT STDMETHODCALLTYPE Invoke(
                       ICoreWebView2*,
                       ICoreWebView2WebMessageReceivedEventArgs* args) override {
                       impl_->on_web_message(args);
                       return S_OK;
                   })

CE_WEBVIEW_HANDLER(ResourceRequested, ICoreWebView2WebResourceRequestedEventHandler,
                   IID_ICoreWebView2WebResourceRequestedEventHandler,
                   HRESULT STDMETHODCALLTYPE Invoke(
                       ICoreWebView2*,
                       ICoreWebView2WebResourceRequestedEventArgs* args) override {
                       impl_->on_resource_requested(args);
                       return S_OK;
                   })

CE_WEBVIEW_HANDLER(ProcessFailedHandler, ICoreWebView2ProcessFailedEventHandler,
                   IID_ICoreWebView2ProcessFailedEventHandler,
                   HRESULT STDMETHODCALLTYPE Invoke(
                       ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) override {
                       COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                           COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                       if (args) args->get_ProcessFailedKind(&kind);
                       // A dead browser process is a broken UI, not a broken
                       // session: the card replaces the page and every export
                       // keeps working.
                       impl_->fail(E_FAIL, L"The WebView2 browser process exited");
                       return S_OK;
                   })

#undef CE_WEBVIEW_HANDLER

} // namespace

void Host::Impl::apply_settings() noexcept {
    if (!settings) return;
    settings->put_IsScriptEnabled(TRUE);
    settings->put_AreDefaultContextMenusEnabled(development_tools() ? TRUE : FALSE);
    settings->put_AreDevToolsEnabled(development_tools() ? TRUE : FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsBuiltInErrorPageEnabled(TRUE);
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);
    // Newer settings only exist on newer runtimes; a missing interface is not an
    // error, it just means that default stands.
    ComPtr<ICoreWebView2Settings3> v3;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(v3.put()))))
        v3->put_AreBrowserAcceleratorKeysEnabled(development_tools() ? TRUE : FALSE);
    ComPtr<ICoreWebView2Settings5> v5;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(v5.put()))))
        v5->put_IsPinchZoomEnabled(FALSE);
    ComPtr<ICoreWebView2Settings6> v6;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(v6.put()))))
        v6->put_IsSwipeNavigationEnabled(FALSE);
}

void Host::Impl::on_environment_ready(HRESULT result, ICoreWebView2Environment* created) noexcept {
    if (state.load() != State::creating_environment) return;   // abandoned by shutdown()
    if (FAILED(result) || !created) {
        fail(result, L"The WebView2 runtime is not available");
        return;
    }
    environment = created;
    state.store(State::creating_controller);
    ComPtr<ControllerReady> callback(new ControllerReady(this));
    const HRESULT hr = environment->CreateCoreWebView2Controller(window, callback.get());
    if (FAILED(hr)) fail(hr, L"Cannot create the WebView2 controller");
}

void Host::Impl::on_controller_ready(HRESULT result, ICoreWebView2Controller* created) noexcept {
    if (state.load() != State::creating_controller) return;
    if (FAILED(result) || !created) {
        fail(result, L"Cannot create the WebView2 controller");
        return;
    }
    controller = created;
    if (FAILED(controller->get_CoreWebView2(webview.put())) || !webview) {
        fail(E_FAIL, L"The WebView2 control has no core view");
        return;
    }
    // Paint the design's own base colour before the first frame, so a resize or a
    // slow first paint never flashes white.
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(controller2.put())))) {
        COREWEBVIEW2_COLOR color{};
        color.A = 255;
        color.R = background_r;
        color.G = background_g;
        color.B = background_b;
        controller2->put_DefaultBackgroundColor(color);
    }
    if (SUCCEEDED(webview->get_Settings(settings.put()))) apply_settings();

    // The asset handler has to be in place before the first navigation, or the
    // page's own requests would leave the process and hit the real shadowtrainer.local.
    {
        ComPtr<ResourceRequested> callback(new ResourceRequested(this));
        EventRegistrationToken token{};
        if (SUCCEEDED(webview->add_WebResourceRequested(callback.get(), &token))) {
            resource_token = token;
            resource_registered = true;
        }
    }
    webview->AddWebResourceRequestedFilter(virtual_host_pattern,
                                            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    {
        ComPtr<MessageReceived> callback(new MessageReceived(this));
        EventRegistrationToken token{};
        if (SUCCEEDED(webview->add_WebMessageReceived(callback.get(), &token))) {
            message_token = token;
            message_registered = true;
        }
    }
    {
        ComPtr<ProcessFailedHandler> callback(new ProcessFailedHandler(this));
        EventRegistrationToken token{};
        if (SUCCEEDED(webview->add_ProcessFailed(callback.get(), &token))) {
            failed_token = token;
            failed_registered = true;
        }
    }

    if (bounds_valid) controller->put_Bounds(bounds);
    // A controller is visible as soon as it exists, so holding it back means
    // saying so explicitly. Leaving it alone does NOT keep it hidden: the page
    // then covers the window the moment it is ready and cuts the startup cover
    // off partway through.
    if (hold_visible) {
        controller->put_IsVisible(FALSE);
    } else {
        controller->put_IsVisible(TRUE);
        revealed = true;
    }
    state.store(State::ready);
    if (FAILED(webview->Navigate(start_url))) fail(E_FAIL, L"Cannot navigate to the embedded UI");
}

void Host::Impl::on_web_message(ICoreWebView2WebMessageReceivedEventArgs* args) noexcept {
    if (!args || !handler) return;
    LPWSTR json = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&json)) || !json) return;
    std::wstring reply;
    try {
        reply = handler(json);
    } catch (...) {
        reply.clear();
    }
    CoTaskMemFree(json);
    if (!reply.empty()) webview->PostWebMessageAsJson(reply.c_str());
}

void Host::Impl::serve(ICoreWebView2WebResourceRequestedEventArgs* args,
                       const std::wstring& path) noexcept {
    const Asset* asset = find_asset(path);
    std::string bytes;
    std::wstring mime = L"text/plain; charset=utf-8";
    bool found = false;
    if (asset) {
        mime = asset->mime;
        found = load_asset(module, *asset, bytes);
    }
    if (!environment) return;
    ComPtr<IStream> stream;
    if (found)
        stream.attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()),
                                        static_cast<UINT>(bytes.size())));
    const std::wstring headers = L"Content-Type: " + mime + L"\r\nCache-Control: no-store\r\n";
    ComPtr<ICoreWebView2WebResourceResponse> response;
    if (FAILED(environment->CreateWebResourceResponse(stream.get(), found ? 200 : 404,
                                                      found ? L"OK" : L"Not Found",
                                                      headers.c_str(), response.put())))
        return;
    args->put_Response(response.get());
}

void Host::Impl::on_resource_requested(ICoreWebView2WebResourceRequestedEventArgs* args) noexcept {
    if (!args) return;
    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(request.put())) || !request) return;
    LPWSTR uri = nullptr;
    if (FAILED(request->get_Uri(&uri)) || !uri) return;
    std::wstring path = uri;
    CoTaskMemFree(uri);
    constexpr size_t prefix = std::size(virtual_host) - 1;
    if (path.compare(0, prefix, virtual_host) != 0) return;
    path.erase(0, prefix);
    const size_t cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) path.resize(cut);
    if (path.empty()) path = L"index.html";
    serve(args, path);
}

// ---------------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------------

Host::Host() : impl_(new Impl) {}

Host::~Host() {
    shutdown();
    delete impl_;
}

bool runtime_available() {
    LPWSTR version = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    const bool present = SUCCEEDED(hr) && version != nullptr;
    if (version) CoTaskMemFree(version);
    return present;
}

bool Host::start(HWND window, HINSTANCE module, bool hold_visible) {
    impl_->window = window;
    impl_->module = module;
    impl_->hold_visible = hold_visible;
    impl_->error.clear();
    impl_->state.store(Impl::State::creating_environment);

    const std::wstring folder = user_data_folder();
    if (folder.empty()) {
        impl_->fail(E_FAIL, L"Cannot locate a writable user data folder for WebView2");
        return false;
    }

    ComPtr<EnvironmentReady> callback(new EnvironmentReady(impl_));
    const HRESULT hr =
        CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr, callback.get());
    if (FAILED(hr)) {
        impl_->fail(hr, L"Cannot start the WebView2 environment");
        return false;
    }
    return true;
}

void Host::set_message_handler(MessageHandler handler) {
    impl_->handler = std::move(handler);
}

void Host::resize(const RECT& client) {
    impl_->bounds = client;
    impl_->bounds_valid = true;
    if (impl_->closing.load() || !impl_->controller) return;
    impl_->controller->put_Bounds(client);
}

void Host::post(const std::wstring& json) {
    if (impl_->closing.load() || impl_->state.load() != Impl::State::ready ||
        !impl_->webview || json.empty())
        return;
    impl_->webview->PostWebMessageAsJson(json.c_str());
}

void Host::reveal() {
    if (impl_->closing.load() || impl_->state.load() != Impl::State::ready) return;
    if (impl_->revealed || !impl_->controller) return;
    impl_->revealed = true;
    impl_->controller->put_IsVisible(TRUE);
}

void Host::execute(const std::wstring& script) {
    if (impl_->closing.load() || impl_->state.load() != Impl::State::ready ||
        !impl_->webview || script.empty())
        return;
    impl_->webview->ExecuteScript(script.c_str(), nullptr);
}

bool Host::ready() const noexcept { return impl_->state.load() == Impl::State::ready; }

bool Host::failed() const noexcept { return impl_->state.load() == Impl::State::failed; }

const std::wstring& Host::error_text() const noexcept { return impl_->error; }

void Host::shutdown() {
    // Mark the session closed first: an in-flight creation callback checks this
    // and drops its result instead of touching a torn-down object, and every
    // other call site stops touching the WebView.
    impl_->closing.store(true);
    impl_->state.store(Impl::State::idle);

    if (impl_->webview) {
        if (impl_->message_registered) impl_->webview->remove_WebMessageReceived(impl_->message_token);
        if (impl_->resource_registered) impl_->webview->remove_WebResourceRequested(impl_->resource_token);
        if (impl_->failed_registered) impl_->webview->remove_ProcessFailed(impl_->failed_token);
    }
    impl_->message_registered = impl_->resource_registered = impl_->failed_registered = false;
    // Close first, release second. Releasing the web view while the controller is
    // still open makes WebView2 start its own teardown, which pumps messages and
    // dispatches them into half-released objects; the fault lands inside the
    // loader's window procedure. This is the order the API documents, and the
    // only one that survives a stop.
    if (impl_->controller) {
            impl_->controller->Close();
    }
    impl_->settings.reset();
    impl_->webview.reset();
    impl_->controller.reset();
    impl_->environment.reset();

    // Deliberately no message drain here. The WebView's child windows outlive
    // their controller for a moment, and dispatching a queued message to one of
    // them after Close() runs that window's own procedure against a torn-down
    // object, which faults inside the loader. The host destroys the window first
    // (which takes the children with it) and only then drains.
    //
    // The data folder stays on disk on purpose: the browser process is still
    // shutting down and may be holding it, and a later load in this same process
    // reuses the same per-pid path. Stale folders are swept at the next startup.
}

} // namespace ce::web
