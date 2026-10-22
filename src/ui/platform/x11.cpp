// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace ekphore {

class NativeEditor::Implementation {
  public:
    explicit Implementation(EditorSession &session) : session(session) {}
    ~Implementation() { detach(); }

    bool attach(const NativeWindowParent &parent) {
        if (parent.api != NativeWindowApi::x11 || !parent.handle)
            return false;
        detach();
        XInitThreads();
        display = parent.display ? static_cast<Display *>(parent.display)
                                 : XOpenDisplay(nullptr);
        owns_display = parent.display == nullptr;
        if (!display)
            return false;
        parent_window = static_cast<Window>(
                reinterpret_cast<std::uintptr_t>(parent.handle));
        const auto [width, height] = session.size();
        child_window = XCreateSimpleWindow(
                display, parent_window, 0, 0, width, height, 0, 0, 0);
        if (!child_window) {
            detach();
            return false;
        }
        XSelectInput(display, child_window,
                ExposureMask | StructureNotifyMask | ButtonPressMask |
                        ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                        FocusChangeMask);
        XMapWindow(display, child_window);
        XFlush(display);
        session.set_visible(true);
        event_thread = std::jthread([this](const std::stop_token stop) {
            while (!stop.stop_requested()) {
                pump_and_paint();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
        return true;
    }

    void detach() {
        if (event_thread.joinable()) {
            event_thread.request_stop();
            event_thread.join();
        }
        if (folder_pid > 0) {
            kill(folder_pid,
                    SIGKILL); // only our own chooser
            waitpid(folder_pid, nullptr, 0);
            folder_pid = 0;
            session.select_instrument_folder({});
        }
        if (folder_pipe >= 0) {
            close(folder_pipe);
            folder_pipe = -1;
        }
        if (display && child_window)
            XDestroyWindow(display, child_window);
        child_window = 0;
        parent_window = 0;
        if (display && owns_display)
            XCloseDisplay(display);
        display = nullptr;
        owns_display = false;
        session.set_visible(false);
    }
    void show() {
        if (display && child_window) {
            XMapWindow(display, child_window);
            XFlush(display);
        }
        session.set_visible(true);
    }
    void hide() {
        if (display && child_window) {
            XUnmapWindow(display, child_window);
            XFlush(display);
        }
        session.set_visible(false);
    }
    void idle() {
        if (session.visible())
            session.invalidate();
    }

    bool attached() const { return display && child_window; }
    void *native_handle() const {
        return reinterpret_cast<void *>(
                static_cast<std::uintptr_t>(child_window));
    }
    std::pair<std::uint32_t, std::uint32_t> size() const {
        return session.size();
    }
    bool set_scale(const float scale) {
        session.set_scale(scale);
        const auto [width, height] = session.size();
        if (display && child_window)
            XResizeWindow(display, child_window, width, height);
        return true;
    }

  private:
    void service_folder_dialog() {
        if (folder_pid > 0) {
            const auto drain = [&] {
                char buffer[1024];
                for (ssize_t count; (count = read(folder_pipe, buffer,
                                             sizeof(buffer))) > 0;)
                    if (folder_result.size() +
                                    static_cast<std::size_t>(count) <=
                            65536)
                        folder_result.append(
                                buffer, static_cast<std::size_t>(count));
            };
            drain();
            int status{};
            if (waitpid(folder_pid, &status, WNOHANG) != folder_pid)
                return;
            drain(); // the chooser may have written more bytes before exiting
            folder_pid = 0;
            close(folder_pipe);
            folder_pipe = -1;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                if (!folder_result.empty() && folder_result.back() == '\n')
                    folder_result.pop_back();
                if (key_dialog)
                    session.select_encryption_key(
                            std::filesystem::u8path(folder_result));
                else
                    session.select_instrument_folder(
                            std::filesystem::u8path(folder_result));
            } else {
                if (key_dialog)
                    session.select_encryption_key({});
                else
                    session.select_instrument_folder({});
            }
            return;
        }
        key_dialog = session.key_selection_requested();
        if (!session.folder_selection_requested() && !key_dialog)
            return;
        int descriptors[2];
        if (pipe(descriptors) != 0) {
            session.folder_selection_failed(
                    "Could not open the folder chooser. Drop an instrument folder "
                    "here instead.");
            return;
        }
        posix_spawn_file_actions_t actions;
        fcntl(descriptors[0], F_SETFD, FD_CLOEXEC);
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(
                &actions, descriptors[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, descriptors[0]);
        posix_spawn_file_actions_addclose(&actions, descriptors[1]);
        char command[] = "zenity", select[] = "--file-selection",
             directory[] = "--directory";
        char title[] = "--title=Choose your instrument folder";
        char key_title[] = "--title=Choose Kontakt private-key file";
        char *arguments[] = { command, select, directory, title, nullptr };
        if (key_dialog) {
            arguments[2] = key_title;
            arguments[3] = nullptr;
        }
        const int error = posix_spawnp(
                &folder_pid, command, &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(descriptors[1]);
        if (error) {
            close(descriptors[0]);
            folder_pid = 0;
            session.folder_selection_failed(
                    "Install Zenity to choose a folder, or drop an instrument "
                    "folder here.");
            return;
        }
        folder_pipe = descriptors[0];
        fcntl(folder_pipe, F_SETFL, fcntl(folder_pipe, F_GETFL) | O_NONBLOCK);
        folder_result.clear();
    }
    void pump_and_paint() {
        if (!display || !child_window)
            return;
        while (XPending(display) != 0) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.xany.window == child_window)
                handle(event);
        }
        if (session.visible()) {
            service_folder_dialog();
            session.invalidate();
            paint();
        }
    }

    void handle(XEvent &event) {
        XWindowAttributes attributes{};
        XGetWindowAttributes(display, child_window, &attributes);
        const auto width =
                static_cast<std::uint32_t>(std::max(attributes.width, 1));
        const auto height =
                static_cast<std::uint32_t>(std::max(attributes.height, 1));
        switch (event.type) {
            case Expose:
                paint();
                break;
            case ButtonPress:
                if (event.xbutton.button == Button4 ||
                        event.xbutton.button == Button5)
                    (void)session.pointer_scroll(event.xbutton.x,
                            event.xbutton.y,
                            event.xbutton.button == Button4 ? -72.0 : 72.0,
                            width, height);
                if (event.xbutton.button == Button1)
                    (void)session.pointer_down(
                            event.xbutton.x, event.xbutton.y, width, height);
                break;
            case MotionNotify:
                if ((event.xmotion.state & Button1Mask) != 0)
                    (void)session.pointer_drag(
                            event.xmotion.x, event.xmotion.y, width, height);
                else
                    (void)session.pointer_move(
                            event.xmotion.x, event.xmotion.y, width, height);
                break;
            case ButtonRelease:
                if (event.xbutton.button == Button1)
                    session.pointer_up();
                break;
            case KeyPress:
                key(event.xkey);
                break;
            default:
                break;
        }
    }

    void key(XKeyEvent &event) {
        KeySym symbol{};
        char utf8[32]{};
        const auto count =
                XLookupString(&event, utf8, sizeof(utf8), &symbol, nullptr);
        const bool shift = (event.state & ShiftMask) != 0;
        const bool control = (event.state & ControlMask) != 0;
        if (const auto translated = translate_key(symbol)) {
            (void)session.key_down(*translated, shift, control);
        } else if (count > 0) {
            (void)session.text_input({ utf8, static_cast<std::size_t>(count) });
        }
    }

    static std::optional<UiKey> translate_key(const KeySym key) {
        switch (key) {
            case XK_Escape:
                return UiKey::escape;
            case XK_BackSpace:
                return UiKey::backspace;
            case XK_Tab:
            case XK_ISO_Left_Tab:
                return UiKey::tab;
            case XK_Return:
            case XK_KP_Enter:
                return UiKey::enter;
            case XK_Delete:
                return UiKey::delete_forward;
            case XK_Home:
                return UiKey::home;
            case XK_End:
                return UiKey::end;
            case XK_Left:
                return UiKey::left;
            case XK_Right:
                return UiKey::right;
            case XK_Up:
                return UiKey::up;
            case XK_Down:
                return UiKey::down;
            default:
                return std::nullopt;
        }
    }

    void paint() {
        if (!display || !child_window)
            return;
        auto frame = session.render();
        XWindowAttributes attributes{};
        XGetWindowAttributes(display, child_window, &attributes);
        const auto destination_width = std::max(attributes.width, 1);
        const auto destination_height = std::max(attributes.height, 1);
        pixels.resize(static_cast<std::size_t>(destination_width) *
                      destination_height * 4);
        for (int y = 0; y < destination_height; ++y) {
            const auto source_y = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(y) * frame.height /
                    destination_height);
            for (int x = 0; x < destination_width; ++x) {
                const auto source_x = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) * frame.width /
                        destination_width);
                const auto source =
                        static_cast<std::size_t>(source_y) * frame.stride +
                        static_cast<std::size_t>(source_x) * 4;
                const auto target =
                        (static_cast<std::size_t>(y) * destination_width + x) *
                        4;
                pixels[target] = frame.rgba[source + 2];
                pixels[target + 1] = frame.rgba[source + 1];
                pixels[target + 2] = frame.rgba[source];
                pixels[target + 3] = 0;
            }
        }
        auto *image = XCreateImage(display,
                DefaultVisual(display, DefaultScreen(display)), 24, ZPixmap, 0,
                reinterpret_cast<char *>(pixels.data()), destination_width,
                destination_height, 32, destination_width * 4);
        if (!image)
            return;
        const auto context = DefaultGC(display, DefaultScreen(display));
        XPutImage(display, child_window, context, image, 0, 0, 0, 0,
                destination_width, destination_height);
        // XDestroyImage would free pixels otherwise
        image->data = nullptr;
        XDestroyImage(image);
        XFlush(display);
    }

    EditorSession &session;
    Display *display{};
    Window parent_window{};
    Window child_window{};
    bool owns_display{};
    std::vector<std::uint8_t> pixels;
    std::jthread event_thread;
    pid_t folder_pid{};
    bool key_dialog{};
    int folder_pipe{ -1 };
    std::string folder_result;
};

NativeEditor::NativeEditor(EditorSession &session)
    : implementation(std::make_unique<Implementation>(session)) {}
NativeEditor::~NativeEditor() = default;
NativeEditor::NativeEditor(NativeEditor &&) noexcept = default;
NativeEditor &NativeEditor::operator=(NativeEditor &&) noexcept = default;
bool NativeEditor::supports(const NativeWindowApi api) {
    return api == NativeWindowApi::x11;
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
