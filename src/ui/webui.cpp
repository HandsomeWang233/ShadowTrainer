#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN keeps the COM declarations out of windows.h.
#include <objbase.h>
#include <commdlg.h>
#include <windowsx.h>

#include "webui.hpp"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

#include "cover.hpp"
#include "hotkey.hpp"
#include "resource_ids.h"
#include "ui_bridge.hpp"
#include "webview_host.hpp"

namespace ce {
namespace {

constexpr UINT show_message = WM_APP + 1;
constexpr UINT stop_message = WM_APP + 2;
constexpr UINT toggle_message = WM_APP + 3;
// The worker posts this when a scan finishes, so the browser hears about the
// result in milliseconds instead of on the next 100 ms tick.
constexpr UINT wake_message = WM_APP + 5;
// Posted by the ticker thread once per composition; see start_ticker.
constexpr UINT tick_message = WM_APP + 6;
// A message that takes at least this long is treated as a stall and its time is
// handed back to the startup cover's clock, so the animation holds the beat
// instead of skipping it. See the message loop.
constexpr ULONGLONG stall_credit_ms = 8;
constexpr UINT_PTR poll_timer = 1, hotkey_timer = 2;
constexpr wchar_t window_property[] = L"SHADOWTRAINER_WINDOW";
constexpr int default_width = 1360, default_height = 900;
constexpr int minimum_width = 1240, minimum_height = 820;
constexpr int resize_border = 6;
constexpr int caption_height = 34;

// Win11 rounds window corners for us; Win10 has to be given a region. Both are
// best-effort: a square window is a cosmetic loss, never a failure.
constexpr DWORD dwm_window_corner_preference = 33;
constexpr int dwm_corner_round = 2;

// The old UI's table dialog, kept as-is: same filter, same default extension,
// and the same title rename on init. A stop that lands while the dialog is up
// is handled once it closes; see choose_table_file.
UINT_PTR CALLBACK table_dialog_hook(HWND dialog, UINT message, WPARAM, LPARAM) {
    if (message == WM_NOTIFY) return 0;
    if (message == WM_INITDIALOG) SetWindowTextW(dialog, L"ShadowTrainer cheat table");
    return 0;
}

// A millisecond clock fine enough to animate against.
//
// GetTickCount64 only moves when the system clock ticks - about every 15.6ms
// unless something in the process has raised the timer resolution - so sampling
// it once per frame hands the animation a time that jumps in steps and sits
// still in between. Every frame then draws either the same instant or one
// several milliseconds on, which is what makes the motion look uneven.
ULONGLONG now_ms() {
    static const double scale = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0
                   ? 1000.0 / static_cast<double>(frequency.QuadPart)
                   : 1.0;
    }();
    LARGE_INTEGER count{};
    QueryPerformanceCounter(&count);
    return static_cast<ULONGLONG>(static_cast<double>(count.QuadPart) * scale);
}

// DwmFlush waits for the next composition. Resolved by hand, like the corner
// call below: dwmapi is not linked, and this module has to load into processes
// whose DWM may be older than the SDK it was built against.
using DwmFlushFn = HRESULT(WINAPI*)();

DwmFlushFn dwm_flush() {
    static DwmFlushFn resolved = []() -> DwmFlushFn {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (!dwm) return nullptr;
        return reinterpret_cast<DwmFlushFn>(
            reinterpret_cast<void*>(GetProcAddress(dwm, "DwmFlush")));
    }();
    return resolved;
}

class WebUi {
public:
    WebUi(HINSTANCE instance, std::atomic<bool>& stop, std::atomic<HWND>& published,
          std::atomic<uint32_t>& runtime_state)
        : module_(instance), stop_requested_(stop), published_(published), state_(runtime_state) {
        session_ = new ui::Session(ui::production_api(), [this] {
            const HWND target = hwnd_;
            if (target) PostMessageW(target, wake_message, 0, 0);
        });
    }

    ~WebUi() {
        published_.store(nullptr);
        stop_ticker();
        release_boot_buffer();
        if (hwnd_) {
            KillTimer(hwnd_, poll_timer);
            KillTimer(hwnd_, hotkey_timer);
        }
        // Idempotent: loop()'s teardown has usually run already, and the early
        // exit paths (create() failed, an exception) still need it.
        host_.shutdown();
        if (session_) {
            session_->request_stop();
            delete session_;
            session_ = nullptr;
        }
        if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(class_name_, module_);
    }

    bool create() {
        std::swprintf(class_name_, std::size(class_name_), L"ShadowTrainer.WebUI.%p.%p",
                      static_cast<void*>(module_), static_cast<void*>(this));
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = window_proc;
        wc.hInstance = module_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        // The window belongs to the host process, so without a class icon the
        // taskbar and Alt+Tab would show the host's own executable.
        wc.hIcon = static_cast<HICON>(LoadImageW(
            module_, MAKEINTRESOURCEW(IDR_APP_ICON), IMAGE_ICON, 0, 0,
            LR_DEFAULTSIZE | LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(
            module_, MAKEINTRESOURCEW(IDR_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
            LR_SHARED));
        // Painted before the first frame so a cold start never flashes white.
        background_brush_ = CreateSolidBrush(background_color());
        wc.hbrBackground = background_brush_;
        wc.lpszClassName = class_name_;
        if (!RegisterClassExW(&wc)) return false;
        registered_ = true;

        const int x = (std::max)(0, (GetSystemMetrics(SM_CXSCREEN) - default_width) / 2);
        const int y = (std::max)(0, (GetSystemMetrics(SM_CYSCREEN) - default_height) / 2);
        hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, class_name_,
                                L"ShadowTrainer",
                                WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                    WS_SYSMENU | WS_CLIPCHILDREN,
                                x, y, default_width, default_height, nullptr, nullptr, module_, this);
        if (!hwnd_) return false;
        // The class icon already covers this window; setting it on the window
        // too keeps Alt+Tab and the taskbar button off the host's own icon.
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));
        if (!SetPropW(hwnd_, window_property, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1))))
            return false;
        fit_work_area();

        // The message handler is installed before the environment starts, so a
        // page that loads fast cannot race the wiring.
        session_->host().window = [this](const std::wstring& command, const json::Value* args) {
            window_command(command, args);
        };
        session_->host().table_dialog = [this](bool save) { return choose_table_file(save); };
        host_.set_message_handler([this](const std::wstring& command) {
            const std::wstring reply = session_->dispatch(command);
            push_events();
            // A page that has just loaded asks "hello" first. If the cover has
            // already played, this is the moment to hand it the go-ahead: the
            // page that was loading when the cover ended missed the first call,
            // and a reload of a page that did not misses it too.
            if (boot_revealed_ && command.find(L"hello") != std::wstring::npos) {
                call_page_reveal();
            }
            return reply;
        });

        RECT client{};
        GetClientRect(hwnd_, &client);
        host_.resize(client);
        if (!SetTimer(hwnd_, poll_timer, 100, nullptr)) return false;
        if (!SetTimer(hwnd_, hotkey_timer, 16, nullptr)) return false;
        published_.store(hwnd_);
        return true;
    }

    int loop() {
        if (stop_requested_.load()) return 0;
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        uint32_t expected = CE_STARTING;
        if (!state_.compare_exchange_strong(expected, CE_RUNNING)) return 0;
        if (stop_requested_.load()) return 0;
        // The WebView is created only now. Its environment completes
        // synchronously, so starting it before the window was on screen left the
        // controller talking to a window that had never been shown: the child
        // never surfaced and the fallback card stayed visible underneath it.
        // A missing runtime is a degraded UI, never a dead session: start()
        // reports it and the card keeps the client area.
        host_.start(hwnd_, module_, /*hold_visible=*/true);
        MSG message{};
        for (;;) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == 0) break;
            if (result == -1) {
                fatal_ = true;
                break;
            }
            // No IsDialogMessage: the browser owns Tab, Enter and the arrow keys,
            // and the backtick has to reach the page rather than being eaten as a
            // dialog accelerator.
            TranslateMessage(&message);
            // The cover is drawn on this thread, so any work that blocks it would
            // otherwise skip that much of the sequence. WebView2's own startup does
            // exactly that about 0.8 s in -- a Chrome_MessageWindow message runs
            // for ~115 ms -- which lands right where the word hands over to the
            // letters, and skipping that stretch popped the first letter into
            // place. Handing the lost time back to the cover's clock turns the
            // jump into a held beat: the next frame continues where the last one
            // stopped. The threshold keeps ordinary messages from stretching the
            // animation.
            const ULONGLONG dispatch_started = now_ms();
            DispatchMessageW(&message);
            const ULONGLONG dispatch_spent = now_ms() - dispatch_started;
            if (boot_started_ != 0 && !boot_revealed_ && dispatch_spent >= stall_credit_ms)
                boot_started_ += dispatch_spent;
            // The stop path has closed and released the WebView by now. Anything
            // still queued -- including messages the WebView posted for its own
            // windows, which live in this process -- would be dispatched into
            // torn-down objects, so the loop ends with the session.
            if (finished_) break;
        }
        teardown();
        return fatal_ ? 1 : 0;
    }

private:
    // Must stay equal to --bg-app in web/css/app.css: the window paints this before
    // the page exists, and the startup veil opens on the same colour.
    static constexpr COLORREF background_color() { return RGB(0x0c, 0x0d, 0x0f); }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        WebUi* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<WebUi*>(create->lpCreateParams);
            self->hwnd_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<WebUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        try {
            return self->handle(message, wparam, lparam);
        } catch (...) {
            // Nothing may escape into the message loop: a failure here has to end
            // the session cleanly instead of taking the host process with it.
            self->stop_requested_.store(true);
            self->session_->request_stop();
            self->fatal_ = true;
            PostQuitMessage(1);
            return 0;
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_NCCALCSIZE:
            if (wparam) return 0;
            break;
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCHITTEST: {
            const POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT bounds{};
            GetWindowRect(hwnd_, &bounds);
            if (!IsZoomed(hwnd_)) {
                const bool left = screen.x < bounds.left + resize_border;
                const bool right = screen.x >= bounds.right - resize_border;
                const bool top = screen.y < bounds.top + resize_border;
                const bool bottom = screen.y >= bounds.bottom - resize_border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            // The page draws its own title bar; this band only has to stay
            // draggable for the frame the fallback card is using.
            if (!host_.ready() && screen.y < bounds.top + caption_height) return HTCAPTION;
            break;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = minimum_width;
            info->ptMinTrackSize.y = minimum_height;
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left;
                info->ptMaxPosition.y = monitor.rcWork.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
            return 0;
        }
        case WM_SIZE: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            host_.resize(client);
            round_corners();
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == poll_timer) {
                if (stop_requested_.load()) {
                    begin_stop();
                    return 0;
                }
                push_events();
            } else if (wparam == hotkey_timer) {
                sample_hotkey();
                pump_boot();
            }
            return 0;
        case wake_message:
            push_events();
            return 0;
        case tick_message:
            // The compositor's beat; see start_ticker.
            if (!boot_revealed_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd_, &paint);
            if (host_.failed()) paint_failure_card(dc);
            else if (!boot_revealed_) paint_boot(dc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            // While the cover owns the window it paints every pixel itself, from
            // a buffer. Erasing here as well would put the bare field on screen a
            // moment before the frame lands on top of it, which flickers.
            if (!boot_revealed_) return 1;
            break;
        case show_message:
            show_only();
            return 0;
        case toggle_message:
            if (IsWindowVisible(hwnd_)) hide_only();
            else show_only();
            return 0;
        case stop_message:
            begin_stop();
            return 0;
        case WM_CLOSE:
            // Hiding is the point: scanning and freezing keep running.
            if (!stopping_) hide_only();
            return 0;
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_CLOSE) {
                if (!stopping_) hide_only();
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(hwnd_, poll_timer);
            KillTimer(hwnd_, hotkey_timer);
            RemovePropW(hwnd_, window_property);
            published_.store(nullptr);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    // ---- window state ---------------------------------------------------------

    void fit_work_area() {
        RECT bounds{};
        if (!GetWindowRect(hwnd_, &bounds)) return;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const int width = (std::min)(bounds.right - bounds.left,
                                     monitor.rcWork.right - monitor.rcWork.left);
        const int height = (std::min)(bounds.bottom - bounds.top,
                                      monitor.rcWork.bottom - monitor.rcWork.top);
        const int x = (std::max)(monitor.rcWork.left,
                                 (std::min)(bounds.left, monitor.rcWork.right - width));
        const int y = (std::max)(monitor.rcWork.top,
                                 (std::min)(bounds.top, monitor.rcWork.bottom - height));
        SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void round_corners() {
        if (IsZoomed(hwnd_)) {
            // Maximised windows are square, and a region would clip their edges.
            if (region_applied_) {
                SetWindowRgn(hwnd_, nullptr, TRUE);
                region_applied_ = false;
            }
            return;
        }
        if (!dwm_tried_) {
            dwm_tried_ = true;
            if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
                using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
                if (auto set = reinterpret_cast<SetAttribute>(
                        GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
                    const int preference = dwm_corner_round;
                    const HRESULT hr = set(hwnd_, dwm_window_corner_preference, &preference,
                                           sizeof(preference));
                    if (SUCCEEDED(hr)) {
                        dwm_rounded_ = true;
                        FreeLibrary(dwm);
                        return;
                    }
                }
                FreeLibrary(dwm);
            }
        }
        if (dwm_rounded_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, 40, 40);
        if (region) {
            SetWindowRgn(hwnd_, region, TRUE);
            region_applied_ = true;
        }
    }

    void hide_only() {
        ShowWindow(hwnd_, SW_HIDE);
    }

    void show_only() {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }

    void begin_stop() {
        hotkey_.disarm();
        if (!stopping_) {
            stopping_ = true;
            stop_requested_.store(true);
            session_->request_stop();
        }
        if (dialog_active_) return;   // the dialog's own loop finishes the teardown
        published_.store(nullptr);
        // No timer may fire into a closing WebView.
        KillTimer(hwnd_, poll_timer);
        KillTimer(hwnd_, hotkey_timer);
        teardown();
        // The teardown itself runs on the message loop's stack; see teardown().
        finished_ = true;
    }

    // Everything that touches the WebView at the end runs here, on the message
    // loop's own stack: Close() pumps messages, and doing that from inside a
    // window-procedure dispatch re-enters WebView2 while it is still mid-flight.
    void teardown() {
        host_.shutdown();
        // The window goes last: destroying it destroys the WebView's child
        // windows too, so no queued message can still reach a closed controller.
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // ---- hotkey ---------------------------------------------------------------

    void sample_hotkey() {
        const bool eligible = !stopping_ && !dialog_active_ && !stop_requested_.load() &&
                              foreground_is_ours();
        const bool down = (GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;
        if (hotkey_.sample(down, eligible)) {
            if (IsWindowVisible(hwnd_)) hide_only();
            else show_only();
        }
    }

    bool foreground_is_ours() const noexcept {
        const HWND foreground = GetForegroundWindow();
        if (!foreground) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        return pid == GetCurrentProcessId();
    }

    // ---- bridge plumbing ------------------------------------------------------

    void push_events() {
        if (!session_) return;
        // The card and the "starting" line are painted by this window, so a
        // change of WebView state has to repaint it: until the page paints, this
        // is what the user is looking at.
        const int state = host_.failed() ? 2 : (host_.ready() ? 1 : 0);
        if (state != painted_state_) {
            painted_state_ = state;
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        const bool visible = hwnd_ && IsWindowVisible(hwnd_);
        const bool minimized = hwnd_ && IsIconic(hwnd_);
        const std::wstring event = session_->pump(visible != 0, minimized != 0);
        if (!event.empty()) host_.post(event);
    }

    void window_command(const std::wstring& name, const json::Value* args) {
        if (name == L"window.drag") {
            start_system_drag(HTCAPTION, args);
        } else if (name == L"window.beginResize") {
            start_system_drag(resize_code(args), args);
        } else if (name == L"window.minimize") {
            ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (name == L"window.maximize") {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (name == L"window.close") {
            hide_only();
        }
    }

    static int resize_code(const json::Value* args) {
        const std::wstring edge = args && args->find(L"edge")
                                      ? std::wstring(args->find(L"edge")->as_string())
                                      : std::wstring();
        if (edge == L"left") return HTLEFT;
        if (edge == L"right") return HTRIGHT;
        if (edge == L"top") return HTTOP;
        if (edge == L"bottom") return HTBOTTOM;
        if (edge == L"topLeft") return HTTOPLEFT;
        if (edge == L"topRight") return HTTOPRIGHT;
        if (edge == L"bottomLeft") return HTBOTTOMLEFT;
        if (edge == L"bottomRight") return HTBOTTOMRIGHT;
        return HTCAPTION;
    }

    // The classic custom-frame hand-off: the browser owns the mouse, so it is
    // released first and Windows' own move/size loop is started by posting the
    // non-client button press with the screen coordinates the page reported.
    void start_system_drag(int code, const json::Value* args) {
        const int x = args && args->find(L"x") ? static_cast<int>(args->find(L"x")->as_i64()) : 0;
        const int y = args && args->find(L"y") ? static_cast<int>(args->find(L"y")->as_i64()) : 0;
        ReleaseCapture();
        PostMessageW(hwnd_, WM_NCLBUTTONDOWN, static_cast<WPARAM>(code), MAKELPARAM(x, y));
    }

    std::wstring choose_table_file(bool save) {
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"Cheat tables (*.CT)\0*.CT\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt = L"CT";
        dialog.lpfnHook = table_dialog_hook;
        dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ENABLEHOOK |
                       (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        // The dialog's own message loop keeps delivering our timers, so the
        // bridge has to refuse commands and stop touching the core meanwhile.
        dialog_active_ = true;
        session_->set_dialog_active(true);
        const BOOL chosen = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
        session_->set_dialog_active(false);
        dialog_active_ = false;
        // A stop requested while the dialog was up could only record itself; the
        // teardown happens here, once the nested loop is gone.
        if (stopping_) begin_stop();
        if (!chosen) return {};
        return path;
    }

    // ---- what the window shows before (or instead of) the page ----------------

    // The startup cover lives in src/ui/cover.cpp, which draws the word sequence
    // this window plays while WebView2 comes up. It is split out so it can be
    // rendered off-screen and looked at; see the note at the top of cover.hpp.

    // Ends the cover once its clock has run out, and hands the window to the
    // page when the WebView is ready.
    void pump_boot() {
        if (boot_revealed_) return;
        if (boot_started_ == 0) return;   // nothing painted yet, so no clock to keep

        if (host_.failed()) {
            boot_revealed_ = true;   // the failure card owns the window from here
            return;
        }
        if (now_ms() - boot_started_ < cover::duration_ms()) return;
        // The word has played out; the page goes up as soon as it is ready.
        if (host_.ready()) reveal_page();
    }

    // Hands the screen to the page: it becomes visible, and is asked to run its
    // own reveal from the centre outwards.
    void reveal_page() {
        stop_ticker();
        boot_revealed_ = true;
        host_.reveal();
        call_page_reveal();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // The page defines this hook. It may not exist yet - the page can still be
    // loading when the cover finishes - in which case the "hello" it sends once
    // it is up re-asks, and its own timeout covers the rest.
    void call_page_reveal() {
        host_.execute(L"window.__shadowtrainerReveal && window.__shadowtrainerReveal()");
    }

    // One frame of the cover. Painted until the page takes the screen.
    void paint_boot(HDC dc) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        // The sequence starts on the first frame the window actually paints. It
        // is created and shown before the WebView is, and letting the timer
        // start the clock would spend the opening on a window nobody can see yet.
        if (boot_started_ == 0) {
            boot_started_ = now_ms();
            start_ticker(hwnd_);
        }
        const ULONGLONG elapsed = now_ms() - boot_started_;
        const RECT area{0, 0, width, height};

        HDC composed = boot_buffer(dc, width, height);
        if (!composed) {
            cover::paint(dc, area, background_brush_, background_color(), cover_state_, elapsed);
            return;
        }
        cover::paint(composed, area, background_brush_, background_color(), cover_state_, elapsed);
        BitBlt(dc, 0, 0, width, height, composed, 0, 0, SRCCOPY);
    }

    // The field the cover is composed on, rebuilt only when the window changes
    // size. Painting straight onto the window instead would show every
    // intermediate state - the field filled, then the glow, then the word - and
    // at sixty frames a second that reads as a flicker.
    HDC boot_buffer(HDC reference, int width, int height) {
        if (width <= 0 || height <= 0) return nullptr;
        if (buffer_dc_ && buffer_width_ == width && buffer_height_ == height) return buffer_dc_;
        release_boot_buffer();
        HDC memory = CreateCompatibleDC(reference);
        if (!memory) return nullptr;
        HBITMAP bitmap = CreateCompatibleBitmap(reference, width, height);
        if (!bitmap) {
            DeleteDC(memory);
            return nullptr;
        }
        SelectObject(memory, bitmap);
        buffer_dc_ = memory;
        buffer_bitmap_ = bitmap;
        buffer_width_ = width;
        buffer_height_ = height;
        return buffer_dc_;
    }

    // Frames are driven by the compositor, not by a timer.
    //
    // SetTimer is only accurate to the system clock tick and WM_TIMER is a
    // low-priority message, so the interval wanders by several milliseconds from
    // frame to frame and the motion looks uneven. The page's copy of this
    // sequence never had that problem because the browser drives it off the
    // composition clock; DwmFlush waits for that same clock from a second
    // thread, which leaves the window's own thread free to paint.
    void start_ticker(HWND window) {
        if (ticking_.load()) return;
        ticking_.store(true);
        ticker_ = std::thread([this, window] {
            while (ticking_.load()) {
                // With composition off - a remote session, say - the flush
                // fails at once, and the sleep stands in for the beat. The
                // pointer is checked before it is called: dwmapi is resolved by
                // hand and may not be there at all.
                const DwmFlushFn flush = dwm_flush();
                if (!flush || FAILED(flush())) Sleep(16);
                if (!ticking_.load()) break;
                PostMessageW(window, tick_message, 0, 0);
            }
        });
    }

    void stop_ticker() {
        ticking_.store(false);
        if (ticker_.joinable()) ticker_.join();
    }

    void release_boot_buffer() {
        if (buffer_dc_) {
            DeleteDC(buffer_dc_);
            buffer_dc_ = nullptr;
        }
        if (buffer_bitmap_) {
            DeleteObject(buffer_bitmap_);
            buffer_bitmap_ = nullptr;
        }
        buffer_width_ = buffer_height_ = 0;
    }


    void paint_failure_card(HDC dc) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRect(dc, &client, background_brush_);
        SetBkMode(dc, TRANSPARENT);
        const int left = 48, width = (std::max)(200, static_cast<int>(client.right) - 96);
        RECT line{left, 96, left + width, client.bottom};
        SetTextColor(dc, RGB(0xe8, 0xe6, 0xe2));
        HFONT title = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        const HGDIOBJ previous = SelectObject(dc, title);
        DrawTextW(dc, L"ShadowTrainer needs the Microsoft Edge WebView2 Runtime", -1, &line,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous);
        DeleteObject(title);
        line.top += 48;
        SetTextColor(dc, RGB(0xa8, 0xa5, 0xa0));
        HFONT body = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        const HGDIOBJ previous_body = SelectObject(dc, body);
        std::wstring text =
            L"The interface cannot be shown without it. Install it from\n"
            L"https://developer.microsoft.com/microsoft-edge/webview2/\n\n";
        if (!host_.error_text().empty()) text += host_.error_text() + L"\n\n";
        text += L"This session is still running: scanning, the address list and freezing all work "
                L"through the CE API. Press ` to hide this window, and Stop session to end the "
                L"session.";
        DrawTextW(dc, text.c_str(), -1, &line, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, previous_body);
        DeleteObject(body);
    }

    HINSTANCE module_ = nullptr;
    std::atomic<bool>& stop_requested_;
    std::atomic<HWND>& published_;
    std::atomic<uint32_t>& state_;
    HWND hwnd_ = nullptr;
    wchar_t class_name_[128]{};
    bool registered_ = false;
    bool fatal_ = false;
    bool finished_ = false;
    bool stopping_ = false;
    bool dialog_active_ = false;
    int painted_state_ = -1;   // last WebView state pushed; see push_events
    bool dwm_tried_ = false;
    bool dwm_rounded_ = false;
    bool region_applied_ = false;
    HBRUSH background_brush_ = nullptr;
    // Startup cover state; see paint_boot and pump_boot. The GDI objects it
    // caches clean themselves up when this is destroyed.
    ULONGLONG boot_started_ = 0;
    bool boot_revealed_ = false;
    cover::State cover_state_;
    std::thread ticker_;
    std::atomic<bool> ticking_{false};
    HDC buffer_dc_ = nullptr;
    HBITMAP buffer_bitmap_ = nullptr;
    int buffer_width_ = 0;
    int buffer_height_ = 0;
    HotkeyState hotkey_;
    web::Host host_;
    ui::Session* session_ = nullptr;
};

} // namespace

int run_ui(HINSTANCE module, std::atomic<bool>& stop_requested,
           std::atomic<HWND>& window, std::atomic<uint32_t>& state) {
    if (stop_requested.load()) return 0;
    // Only this dedicated thread touches the DPI context or the COM apartment;
    // the host process is left exactly as it was.
    struct DpiScope {
        DPI_AWARENESS_CONTEXT previous =
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
    } dpi;
    struct ComScope {
        // S_FALSE still means "this thread owns an initialization". A changed
        // mode means the host already put this thread in the MTA: WebView2 cannot
        // run there, and we must not uninitialize an apartment we do not own.
        bool owned = false;
        ComScope() {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            owned = SUCCEEDED(hr);
        }
        ~ComScope() { if (owned) CoUninitialize(); }
    } com;
    try {
        WebUi ui(module, stop_requested, window, state);
        if (!ui.create()) {
            if (stop_requested.load()) return 0;
            uint32_t starting = CE_STARTING;
            state.compare_exchange_strong(starting, CE_FAILED);
            OutputDebugStringW(L"ShadowTrainer WebUI: window initialization failed.\n");
            return 1;
        }
        return ui.loop();
    } catch (...) {
        window.store(nullptr);
        if (stop_requested.load()) return 0;
        uint32_t starting = CE_STARTING;
        state.compare_exchange_strong(starting, CE_FAILED);
        OutputDebugStringW(L"ShadowTrainer WebUI: initialization or message-loop exception.\n");
        return 1;
    }
}

} // namespace ce
