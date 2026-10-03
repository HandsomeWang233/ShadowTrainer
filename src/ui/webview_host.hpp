#pragma once
// WebView2 plumbing for the web UI, and nothing else: the environment and
// controller, the embedded-asset server, the message channel, and the shutdown
// sequence that lets the DLL unload.
//
// Everything above this file is the host (src/ui/webui.cpp, which owns the window
// and the timer) and the bridge (src/bridge/ui_bridge.cpp, which owns the semantics).
// This class never interprets a command and never touches the core.
//
// The header stays free of WebView2.h on purpose: the SDK header is 3 MB of COM
// declarations and only this translation unit should pay for it.
#include <functional>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ce::web {

// Handles one inbound JSON message and returns the JSON reply to post back.
using MessageHandler = std::function<std::wstring(const std::wstring& command)>;

class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Starts the asynchronous environment creation. Returns false when no
    // writable user data folder can be located, or the environment cannot be
    // created; an asynchronous failure after that is reported through failed().
    //
    // With hold_visible the controller stays hidden once it is ready, so the
    // window can play its own startup cover without the page appearing behind
    // it; reveal() then puts it on screen. Without it the page is shown the
    // moment it is ready.
    //
    // COM must already be initialized on the calling (UI) thread, and the
    // calling thread must keep pumping messages: every completion callback
    // arrives on it.
    bool start(HWND window, HINSTANCE module, bool hold_visible = false);

    // Shows a controller that start() was asked to hold back, and is a no-op
    // for one that was not. Safe to call more than once.
    void reveal();

    // Runs a script in the page. Ignored until the page is live.
    void execute(const std::wstring& script);

    void set_message_handler(MessageHandler handler);
    // Ignored once shutdown() has begun: Close() pumps messages, so window
    // callbacks can still arrive while the controller is going away.
    void resize(const RECT& client);
    // Posts JSON to the page. Ignored until the page is live.
    void post(const std::wstring& json);

    bool ready() const noexcept;
    bool failed() const noexcept;
    // Human-readable reason for failed(), including the HRESULT.
    const std::wstring& error_text() const noexcept;

    // Releases the controller and drains its asynchronous teardown. Must be
    // called on the UI thread, while messages are still being pumped, before
    // the window is destroyed and before CoUninitialize.
    void shutdown();

    // Defined in the .cpp so WebView2.h stays out of this header. Public
    // because the COM callbacks and the completion handlers are free types in
    // that translation unit.
    struct Impl;

private:
    Impl* impl_;
};

// True when the WebView2 runtime is installed machine-wide for this user. A
// cheap check for callers that want to decide before creating a window; start()
// reports the same thing with more detail.
bool runtime_available();

} // namespace ce::web
