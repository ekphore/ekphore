// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace ekphore {

class NativeEditor::Implementation {
  public:
    explicit Implementation(EditorSession &session) : session(session) {}
    ~Implementation() { detach(); }

    bool attach(const NativeWindowParent &parent) {
        if (parent.api != NativeWindowApi::win32 || !parent.handle)
            return false;
        detach();
        register_class();
        const auto [width, height] = session.size();
        child_window = CreateWindowExW(0, class_name, L"",
                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, static_cast<int>(width),
                static_cast<int>(height), static_cast<HWND>(parent.handle),
                nullptr, module(), this);
        if (!child_window)
            return false;
        SetTimer(child_window, 1, 16, nullptr);
        session.set_visible(true);
        InvalidateRect(child_window, nullptr, FALSE);
        return true;
    }

    void detach() {
        if (child_window) {
            KillTimer(child_window, 1);
            DestroyWindow(std::exchange(child_window, nullptr));
        }
        session.set_visible(false);
    }
    void show() {
        if (child_window)
            ShowWindow(child_window, SW_SHOW);
        session.set_visible(true);
    }
    void hide() {
        if (child_window)
            ShowWindow(child_window, SW_HIDE);
        session.set_visible(false);
    }
    void idle() {
        if (!child_window || !session.visible())
            return;
        session.invalidate();
        InvalidateRect(child_window, nullptr, FALSE);
    }
    bool attached() const { return child_window != nullptr; }
    void *native_handle() const { return child_window; }
    std::pair<std::uint32_t, std::uint32_t> size() const {
        return session.size();
    }
    bool set_scale(const float scale) {
        session.set_scale(scale);
        const auto [width, height] = session.size();
        if (child_window) {
            SetWindowPos(child_window, nullptr, 0, 0, static_cast<int>(width),
                    static_cast<int>(height), SWP_NOMOVE | SWP_NOZORDER);
            InvalidateRect(child_window, nullptr, FALSE);
        }
        return true;
    }

  private:
    static constexpr wchar_t class_name[] = L"EkphoreEmbeddedEditor";

    static HINSTANCE module() { return GetModuleHandleW(nullptr); }
    static void register_class() {
        static std::once_flag once;
        std::call_once(once, [] {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.style = CS_HREDRAW | CS_VREDRAW;
            window_class.lpfnWndProc = &Implementation::window_proc;
            window_class.hInstance = module();
            window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
            window_class.lpszClassName = class_name;
            RegisterClassExW(&window_class);
        });
    }

    static Implementation *owner(HWND window) {
        return reinterpret_cast<Implementation *>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    static LRESULT CALLBACK window_proc(
            HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto *self = owner(window);
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        return self->handle(window, message, wparam, lparam);
    }

    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        const auto width =
                static_cast<std::uint32_t>(std::max(bounds.right, 1L));
        const auto height =
                static_cast<std::uint32_t>(std::max(bounds.bottom, 1L));
        const float x = static_cast<float>(GET_X_LPARAM(lparam));
        const float y = static_cast<float>(GET_Y_LPARAM(lparam));
        switch (message) {
            case WM_TIMER:
                if (!folder_dialog_open && session.key_selection_requested()) {
                    folder_dialog_open = true;
                    wchar_t filename[32768]{};
                    OPENFILENAMEW dialog{};
                    dialog.lStructSize = sizeof(dialog);
                    dialog.hwndOwner = window;
                    dialog.lpstrFile = filename;
                    dialog.nMaxFile = 32768;
                    dialog.lpstrTitle = L"Choose Kontakt private-key file";
                    dialog.lpstrFilter = L"All files\0*.*\0";
                    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                                   OFN_NOCHANGEDIR;
                    std::optional<std::filesystem::path> path;
                    if (GetOpenFileNameW(&dialog))
                        path = filename;
                    session.select_encryption_key(path);
                    folder_dialog_open = false;
                }
                if (!folder_dialog_open &&
                        session.folder_selection_requested()) {
                    folder_dialog_open = true;
                    const auto initialized =
                            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    BROWSEINFOW browse{};
                    browse.hwndOwner = window;
                    browse.lpszTitle =
                            L"Choose the folder containing your instruments";
                    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    std::optional<std::filesystem::path> path;
                    if (auto *selected = SHBrowseForFolderW(&browse)) {
                        wchar_t buffer[MAX_PATH]{};
                        if (SHGetPathFromIDListW(selected, buffer))
                            path = buffer;
                        CoTaskMemFree(selected);
                    }
                    if (SUCCEEDED(initialized))
                        CoUninitialize();
                    session.select_instrument_folder(path);
                    folder_dialog_open = false;
                }
                session.invalidate();
                invalidate(window);
                return 0;
            case WM_PAINT:
                paint(window);
                return 0;
            case WM_MOUSEWHEEL: {
                POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                ScreenToClient(window, &point);
                UINT lines = 3;
                SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                if (session.pointer_scroll(point.x, point.y,
                            -GET_WHEEL_DELTA_WPARAM(wparam) *
                                    (lines == WHEEL_PAGESCROLL ? height
                                                               : lines * 24.0) /
                                    WHEEL_DELTA,
                            width, height))
                    invalidate(window);
                return 0;
            }
            case WM_LBUTTONDOWN:
                SetCapture(window);
                SetFocus(window);
                if (session.pointer_down(x, y, width, height))
                    invalidate(window);
                return 0;
            case WM_MOUSEMOVE:
                if ((wparam & MK_LBUTTON) != 0) {
                    if (session.pointer_drag(x, y, width, height))
                        invalidate(window);
                } else if (session.pointer_move(x, y, width, height)) {
                    invalidate(window);
                }
                return 0;
            case WM_LBUTTONUP:
                ReleaseCapture();
                session.pointer_up();
                invalidate(window);
                return 0;
            case WM_CHAR: {
                wchar_t wide[2]{ static_cast<wchar_t>(wparam), 0 };
                char utf8[8]{};
                const auto count = WideCharToMultiByte(CP_UTF8, 0, wide, 1,
                        utf8, sizeof(utf8), nullptr, nullptr);
                if (count > 0 && session.text_input({ utf8,
                                         static_cast<std::size_t>(count) }))
                    invalidate(window);
                return 0;
            }
            case WM_KEYDOWN:
                if (const auto key = translate_key(wparam)) {
                    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    const bool control =
                            (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (session.key_down(*key, shift, control))
                        invalidate(window);
                    return 0;
                }
                break;
            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static std::optional<UiKey> translate_key(const WPARAM key) {
        switch (key) {
            case VK_ESCAPE:
                return UiKey::escape;
            case VK_BACK:
                return UiKey::backspace;
            case VK_TAB:
                return UiKey::tab;
            case VK_RETURN:
                return UiKey::enter;
            case VK_DELETE:
                return UiKey::delete_forward;
            case VK_HOME:
                return UiKey::home;
            case VK_END:
                return UiKey::end;
            case VK_LEFT:
                return UiKey::left;
            case VK_RIGHT:
                return UiKey::right;
            case VK_UP:
                return UiKey::up;
            case VK_DOWN:
                return UiKey::down;
            default:
                return std::nullopt;
        }
    }

    static void invalidate(HWND window) {
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint(HWND window) {
        PAINTSTRUCT paint_state{};
        auto context = BeginPaint(window, &paint_state);
        auto frame = session.render();
        pixels.resize(frame.rgba.size());
        for (std::size_t index = 0; index + 3 < frame.rgba.size(); index += 4) {
            pixels[index] = frame.rgba[index + 2];
            pixels[index + 1] = frame.rgba[index + 1];
            pixels[index + 2] = frame.rgba[index];
            pixels[index + 3] = frame.rgba[index + 3];
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(frame.width);
        info.bmiHeader.biHeight = -static_cast<LONG>(frame.height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        RECT bounds{};
        GetClientRect(window, &bounds);
        StretchDIBits(context, 0, 0, bounds.right, bounds.bottom, 0, 0,
                frame.width, frame.height, pixels.data(), &info, DIB_RGB_COLORS,
                SRCCOPY);
        EndPaint(window, &paint_state);
    }

    EditorSession &session;
    HWND child_window{};
    bool folder_dialog_open{};
    std::vector<std::uint8_t> pixels;
};

NativeEditor::NativeEditor(EditorSession &session)
    : implementation(std::make_unique<Implementation>(session)) {}
NativeEditor::~NativeEditor() = default;
NativeEditor::NativeEditor(NativeEditor &&) noexcept = default;
NativeEditor &NativeEditor::operator=(NativeEditor &&) noexcept = default;
bool NativeEditor::supports(const NativeWindowApi api) {
    return api == NativeWindowApi::win32;
}
bool NativeEditor::attach(const NativeWindowParent &parent) {
    return implementation->attach(parent);
}
void NativeEditor::detach() { implementation->detach(); }
void NativeEditor::show() { implementation->show(); }
void NativeEditor::hide() { implementation->hide(); }
void NativeEditor::idle() { implementation->idle(); }
bool NativeEditor::attached() const { return implementation->attached(); }
void *NativeEditor::native_handle() const {
    return implementation->native_handle();
}
std::pair<std::uint32_t, std::uint32_t> NativeEditor::size() const {
    return implementation->size();
}
bool NativeEditor::set_scale(const float scale) {
    return implementation->set_scale(scale);
}

} // namespace ekphore
