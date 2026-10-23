// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../realtime/queues/buffered_spsc_queue.h"
#include "../realtime/queues/spsc_queue.h"
#include "../ui/browser.h"
#include "app_icon.h"
#include "standalone_midi.h"
#include <ekphore/plugin.h>

#include <SDL3/SDL.h>
#include <png.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL_main.h>
#include <io.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

#ifdef _WIN32
void attach_terminal() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS) &&
            GetLastError() != ERROR_ACCESS_DENIED)
        return;
    const auto reopen = [](FILE *stream) {
        const auto fd = _fileno(stream);
        // don't replace a redirected file or pipe with CONOUT$
        if (fd >= 0 && _get_osfhandle(fd) != -1)
            return;
        FILE *file{};
        (void)freopen_s(&file, "CONOUT$", "w", stream);
    };
    reopen(stdout);
    reopen(stderr);
    std::cout.clear();
    std::cerr.clear();
}
#endif

double wheel_scroll_pixels(const SDL_MouseWheelEvent &wheel, double units) {
    // SDL already flipped it if the OS wants, we negate because our offsets go
    // down
    return -static_cast<double>(wheel.y) * units;
}

struct ArrayDialogReply {
    std::mutex mutex;
    bool ready{};
    std::optional<std::filesystem::path> path;
    std::string error;
};
void SDLCALL array_dialog_done(void *userdata, const char *const *files, int) {
    std::unique_ptr<std::shared_ptr<ArrayDialogReply>> retained(
            static_cast<std::shared_ptr<ArrayDialogReply> *>(userdata));
    std::scoped_lock lock((*retained)->mutex);
    if (files && files[0])
        (*retained)->path = std::filesystem::u8path(files[0]);
    if (!files)
        (*retained)->error = SDL_GetError();
    (*retained)->ready = true;
}

void set_window_icon(SDL_Window *window) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, ekphore::app_icon::png,
                sizeof(ekphore::app_icon::png))) {
        png_image_free(&image);
        return;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> rgba(PNG_IMAGE_SIZE(image));
    const bool okay = png_image_finish_read(
            &image, nullptr, rgba.data(), 0, nullptr);
    png_image_free(&image);
    if (!okay)
        return;
    auto *icon = SDL_CreateSurfaceFrom(int(image.width), int(image.height),
            SDL_PIXELFORMAT_RGBA32, rgba.data(),
            int(PNG_IMAGE_ROW_STRIDE(image)));
    if (!icon)
        return;
    // SDL keeps its own copy, so the surface and rgba can go right away
    (void)SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
}

class Window {
  public:
    ~Window() {
        if (texture)
            SDL_DestroyTexture(texture);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }

    bool open(const ekphore::UiDocument &document, const bool audio = true) {
        if (window) {
            if (audio && !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
                    !SDL_InitSubSystem(SDL_INIT_AUDIO))
                return false;
            SDL_SetWindowTitle(window, document.title.c_str());
            resize(document);
            return true;
        }
        // wayland docks find our icon through the .desktop file named after this id
        (void)SDL_SetAppMetadata("ekphore", nullptr, "io.github.ekphore");
        if (!SDL_Init(SDL_INIT_VIDEO | (audio ? SDL_INIT_AUDIO : 0))) {
            std::cerr << "SDL initialization failed: " << SDL_GetError()
                      << '\n';
            return false;
        }
        bool created = create_window(document);
        // wayland can't put a software framebuffer on screen, so without a GL,
        // EGL or vulkan library we can load there's nothing to draw with. X11
        // can, so go through XWayland unless someone picked a driver themselves
        if (!created && window && current_video_driver() == "wayland" &&
                !SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) {
            std::cerr << "No Wayland renderer (" << SDL_GetError()
                      << "), retrying with X11\n";
            SDL_DestroyWindow(window);
            window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            created = SDL_InitSubSystem(SDL_INIT_VIDEO) && create_window(document);
        }
        if (!created) {
            report_render_failure();
            return false;
        }
        // no vsync, it blocks input and the main loop already throttles
        (void)SDL_SetRenderVSync(renderer, 0);
        SDL_ShowWindow(window);
        return true;
    }

    bool present(const ekphore::UiFrame &frame) {
        const auto started = std::chrono::steady_clock::now();
        if (!renderer || frame.width == 0 || frame.height == 0)
            return false;
        if (!texture || texture_width != frame.width ||
                texture_height != frame.height) {
            if (texture)
                SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                    SDL_TEXTUREACCESS_STREAMING, static_cast<int>(frame.width),
                    static_cast<int>(frame.height));
            texture_width = frame.width;
            texture_height = frame.height;
        }
        if (!texture || !SDL_UpdateTexture(texture, nullptr, frame.rgba.data(),
                                static_cast<int>(frame.stride)))
            return false;
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        if (std::getenv("EKPHORE_UI_TIMING")) {
            present_time += std::chrono::steady_clock::now() - started;
            if (++present_count % 30 == 0) {
                std::cerr << "ui-timing present-us="
                          << std::chrono::duration_cast<
                                     std::chrono::microseconds>(present_time)
                                             .count() /
                                     present_count
                          << '\n';
                present_time = {};
                present_count = 0;
            }
        }
        return true;
    }

    bool wait_event(SDL_Event &event, const int timeout_ms) {
        const bool received =
                timeout_ms < 0 ? SDL_WaitEvent(&event)
                               : SDL_WaitEventTimeout(&event, timeout_ms);
        if (received)
            prepare_event(event);
        return received;
    }

    bool poll_event(SDL_Event &event) {
        if (!SDL_PollEvent(&event))
            return false;
        prepare_event(event);
        return true;
    }
    void prepare_event(SDL_Event &event) {
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED &&
                event.window.windowID == SDL_GetWindowID(window))
            update_display_scale();
        coalesce_hover(event);
    }
    static void coalesce_hover(SDL_Event &event) {
        // only merge plain hover motion, and don't pump or continuous motion
        // keeps us in here forever
        if (event.type != SDL_EVENT_MOUSE_MOTION || event.motion.state != 0)
            return;
        SDL_Event next{};
        while (SDL_PeepEvents(&next, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                       SDL_EVENT_LAST) == 1 &&
                next.type == SDL_EVENT_MOUSE_MOTION && next.motion.state == 0 &&
                next.motion.windowID == event.motion.windowID &&
                next.motion.which == event.motion.which) {
            if (SDL_PeepEvents(&next, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) != 1)
                break;
            next.motion.xrel += event.motion.xrel;
            next.motion.yrel += event.motion.yrel;
            event = next;
        }
    }
    void key_dialog(const std::shared_ptr<ArrayDialogReply> &reply) const {
        auto *retained = new std::shared_ptr<ArrayDialogReply>(reply);
        SDL_ShowOpenFileDialog(array_dialog_done, retained, window, nullptr, 0,
                nullptr, false);
    }
    void folder_dialog(const std::shared_ptr<ArrayDialogReply> &reply,
            const std::filesystem::path &initial) const {
        auto *retained = new std::shared_ptr<ArrayDialogReply>(reply);
        const auto utf8 = initial.u8string();
        SDL_ShowOpenFolderDialog(array_dialog_done, retained, window,
                initial.empty() ? nullptr
                                : reinterpret_cast<const char *>(utf8.c_str()),
                false);
    }
    void array_dialog(const ekphore::Player::ArrayFileRequest &request,
            const std::shared_ptr<ArrayDialogReply> &reply) const {
        static const SDL_DialogFileFilter filter{ "Kontakt arrays", "nka" };
        auto *retained = new std::shared_ptr<ArrayDialogReply>(reply);
        const auto filename = request.array_name.substr(1) + ".nka";
        if (request.save)
            SDL_ShowSaveFileDialog(array_dialog_done, retained, window, &filter,
                    1, filename.c_str());
        else
            SDL_ShowOpenFileDialog(array_dialog_done, retained, window, &filter,
                    1, nullptr, false);
    }

    void set_text_input(const bool enabled) const {
        if (!window)
            return;
        if (enabled)
            (void)SDL_StartTextInput(window);
        else
            (void)SDL_StopTextInput(window);
    }

    std::pair<float, float> document_position(float window_x, float window_y,
            const ekphore::UiDocument &document) const {
        int width{};
        int height{};
        // same units as the mouse, backing pixels can differ on macOS/Wayland
        SDL_GetWindowSize(window, &width, &height);
        return { window_x * document.width / std::max(width, 1),
            window_y * document.height / std::max(height, 1) };
    }

    float display_scale() const {
        if (!window)
            return 1.0F;
        const auto scale = SDL_GetWindowDisplayScale(window);
        return std::isfinite(scale) && scale > 0 ? scale : 1.0F;
    }

  private:
    bool create_window(const ekphore::UiDocument &document) {
        window = SDL_CreateWindow(document.title.c_str(), document.width,
                document.height,
                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                        SDL_WINDOW_HIDDEN);
        if (!window)
            return false;
        set_window_icon(window);
        resize(document);
        SDL_SetWindowPosition(
                window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        renderer = SDL_CreateRenderer(window, nullptr);
        return renderer != nullptr;
    }

    static std::string_view current_video_driver() {
        const char *name = SDL_GetCurrentVideoDriver();
        return name ? name : "";
    }

    void report_render_failure() const {
        std::cerr << (window ? "Renderer" : "Window") << " creation failed: "
                  << SDL_GetError() << "\nvideo driver: "
                  << current_video_driver() << ", render drivers:";
        for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i)
            std::cerr << ' ' << SDL_GetRenderDriver(i);
        std::cerr << '\n';
    }

    float window_scale() const {
        const auto density = SDL_GetWindowPixelDensity(window);
        // Windows uses pixels for window sizes; macOS already uses points
        return display_scale() /
               (std::isfinite(density) && density > 0 ? density : 1.0F);
    }
    void resize(const ekphore::UiDocument &document) {
        coordinate_scale = window_scale();
        SDL_SetWindowSize(window,
                std::max(
                        1, int(std::lround(document.width * coordinate_scale))),
                std::max(1,
                        int(std::lround(document.height * coordinate_scale))));
    }
    void update_display_scale() {
        const auto scale = window_scale();
        if (std::abs(scale - coordinate_scale) < 0.001F)
            return;
        int width{}, height{};
        SDL_GetWindowSize(window, &width, &height);
        const auto ratio = scale / coordinate_scale;
        coordinate_scale = scale;
        SDL_SetWindowSize(window, std::max(1, int(std::lround(width * ratio))),
                std::max(1, int(std::lround(height * ratio))));
    }
    SDL_Window *window{};
    SDL_Renderer *renderer{};
    SDL_Texture *texture{};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    float coordinate_scale{ 1.0F };
    std::chrono::steady_clock::duration present_time{};
    std::size_t present_count{};
};

class StandaloneRuntime {
    friend struct StandaloneMidiInspection;

  public:
    explicit StandaloneRuntime(ekphore::Plugin &plugin)
        : plugin(plugin),
          diagnostics(std::getenv("EKPHORE_UI_DIAGNOSTICS") != nullptr) {}

    ~StandaloneRuntime() {
        midi_worker.request_stop();
        if (midi_worker.joinable())
            midi_worker.join();
        if (audio_stream)
            SDL_DestroyAudioStream(audio_stream);
        plugin.deactivate();
    }

    bool start() {
        constexpr SDL_AudioSpec requested{ SDL_AUDIO_F32, 2, 48000 };
        plugin.activate(requested.freq, 1024);
        audio_stream =
                SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                        &requested, &StandaloneRuntime::provide_audio, this);
        if (!audio_stream) {
            std::cerr << "Audio device creation failed: " << SDL_GetError()
                      << '\n';
            return false;
        }
        if (!SDL_ResumeAudioStreamDevice(audio_stream)) {
            std::cerr << "Audio device start failed: " << SDL_GetError()
                      << '\n';
            return false;
        }
        midi_worker = std::jthread([this](std::stop_token stop) {
            ekphore::standalone::MidiInputs inputs;
            const bool trace = std::getenv("EKPHORE_MIDI_TRACE") != nullptr;
            std::size_t last_admitted{};
            std::size_t received{};
            auto next_report = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                inputs.poll([&](const ekphore::PluginMidiEvent &event) {
                    ++received;
                    midi_events.push(event);
                });
                midi_events.flush();
                if (trace) {
                    const auto admitted =
                            admitted_events.load(std::memory_order_relaxed);
                    if (admitted != last_admitted) {
                        std::cerr << "MIDI audio: " << admitted
                                  << " events delivered; output="
                                  << (midi_audio_audible.load(
                                              std::memory_order_relaxed)
                                                     ? "audible"
                                                     : "silent")
                                  << '\n';
                        last_admitted = admitted;
                    }
                    if (std::chrono::steady_clock::now() >= next_report) {
                        next_report = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
                        std::cerr << "MIDI pipeline: received=" << received
                                  << " admitted=" << admitted
                                  << " audio-blocks="
                                  << midi_audio_blocks.load(
                                             std::memory_order_relaxed)
                                  << " rejected-blocks="
                                  << midi_rejected_blocks.load(
                                             std::memory_order_relaxed)
                                  << " output-peak="
                                  << midi_audio_peak.exchange(
                                             0.0F, std::memory_order_relaxed)
                                  << " audio-error="
                                  << has_audio_error.load(
                                             std::memory_order_relaxed)
                                  << '\n';
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        return true;
    }

    ekphore::UiDocument editor_document() const {
        return plugin.editor_document();
    }

    ekphore::UiFrame render_editor(const float scale = 1.0F) const {
        const auto started = std::chrono::steady_clock::now();
        auto frame = plugin.render_editor(scale);
        record_timing("render", started, render_time, render_count);
        return frame;
    }

    bool pointer_down(const std::int32_t x, const std::int32_t y) {
        return plugin.pointer_down(x, y);
    }

    bool pointer_drag(const std::int32_t x, const std::int32_t y) {
        const auto started = std::chrono::steady_clock::now();
        const auto changed = plugin.pointer_drag(x, y);
        record_timing("drag", started, drag_time, drag_count);
        return changed;
    }

    bool pointer_move(const std::int32_t x, const std::int32_t y) {
        return plugin.pointer_move(x, y);
    }

    bool pointer_scroll(
            const std::int32_t x, const std::int32_t y, const double pixels) {
        return plugin.pointer_scroll(x, y, pixels);
    }
    bool menu_open() const { return plugin.menu_open(); }
    bool menu_animating() const { return plugin.menu_animating(); }

    bool text_input(const std::string_view text) {
        return plugin.text_input(text);
    }

    bool key_down(
            const ekphore::UiKey key, const bool shift, const bool control) {
        return plugin.key_down(key, shift, control);
    }

    bool text_editing_active() const { return plugin.text_editing_active(); }

    void pointer_up() { plugin.pointer_up(); }
    bool service_array_dialog(const Window &window) {
        const bool repaint = array_operation_active;
        if (array_operation_active &&
                !plugin.array_file_active(array_dialog_command))
            array_operation_active = false;
        if (array_dialog_reply) {
            std::scoped_lock lock(array_dialog_reply->mutex);
            if (array_dialog_reply->ready) {
                (void)plugin.select_array_file(
                        array_dialog_command, array_dialog_reply->path);
                array_operation_active = true;
                // don't reset the reply while its mutex is still locked
            } else
                return repaint;
        }
        array_dialog_reply.reset();
        const auto requests = plugin.array_file_requests();
        if (requests.empty())
            return repaint;
        array_dialog_command = requests.front().command;
        array_dialog_reply = std::make_shared<ArrayDialogReply>();
        window.array_dialog(requests.front(), array_dialog_reply);
        return repaint;
    }

    bool advance_preview_clock() {
        const auto now = std::chrono::steady_clock::now();
        const auto frames =
                std::chrono::duration<double>(now - preview_clock).count() *
                48000.0;
        preview_clock = now;
        return plugin.advance_ui_preview_clock(
                static_cast<std::size_t>(frames));
    }

    bool drop_files(const std::int32_t x, const std::int32_t y,
            const std::span<const std::filesystem::path> files,
            const std::uint32_t modifiers) {
        return plugin.drop_files(x, y, files, modifiers);
    }

    void note(const std::uint8_t number, const bool down) {
        if (number >= held_notes.size())
            return;
        if (down == held_notes[number])
            return;
        held_notes[number] = down;
        if (std::getenv("EKPHORE_UI_DIAGNOSTICS"))
            std::cerr << "standalone-note=" << static_cast<int>(number)
                      << (down ? " on" : " off") << '\n';
        events.push({ down ? ekphore::PluginMidiKind::note_on
                           : ekphore::PluginMidiKind::note_off,
                0, 0, number, down ? std::uint8_t{ 100 } : std::uint8_t{ 0 } });
    }

    void release_held_notes() {
        for (std::size_t note_number = 0; note_number < held_notes.size();
                ++note_number)
            if (held_notes[note_number])
                note(static_cast<std::uint8_t>(note_number), false);
    }

    bool input_pending() const { return events.pending(); }

    bool audio_failed() {
        // retry UI overflow even when no new key events arrive
        events.flush();
        // print here, audio only queues them and drops when full
        AudioReport report;
        while (audio_reports.pop(report)) {
            if (report.became_audible)
                std::cerr << "audio became non-zero\n";
            if (report.missed_deadline)
                std::cerr << "audio deadline missed: frames=" << report.frames
                          << " elapsed_ms=" << report.elapsed_ns / 1000000.0
                          << " budget_ms=" << report.frames / 48.0 << '\n';
        }
        const auto lost =
                lost_audio_reports.exchange(0, std::memory_order_relaxed);
        if (lost)
            std::cerr << "audio diagnostic reports dropped=" << lost << '\n';
        return has_audio_error.load();
    }

  private:
    static void record_timing(const char *stage,
            const std::chrono::steady_clock::time_point started,
            std::chrono::steady_clock::duration &total, std::size_t &count) {
        if (!std::getenv("EKPHORE_UI_TIMING"))
            return;
        total += std::chrono::steady_clock::now() - started;
        if (++count % 30 != 0)
            return;
        std::cerr << "ui-timing " << stage << "-us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(
                             total)
                                     .count() /
                             count
                  << '\n';
        total = {};
        count = 0;
    }

    static void SDLCALL provide_audio(void *userdata, SDL_AudioStream *stream,
            const int additional_amount, int) {
        static_cast<StandaloneRuntime *>(userdata)->render_audio(
                stream, additional_amount);
    }

    void render_audio(SDL_AudioStream *stream, const int requested_bytes) {
        const auto callback_started =
                diagnostics ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};
        constexpr int frame_bytes = 2 * static_cast<int>(sizeof(float));
        const auto frame_count = static_cast<std::size_t>(
                std::max(requested_bytes, 0) / frame_bytes);
        if (frame_count == 0)
            return;

        pending_event_count += events.drain(
                std::span(audio_events).subspan(pending_event_count));
        pending_event_count += midi_events.drain(
                std::span(audio_events).subspan(pending_event_count));
        bool became_audible = false;
        for (std::size_t offset = 0; offset < frame_count;) {
            const auto count =
                    std::min(frame_count - offset, sample_left.size());
            auto left = std::span(sample_left).first(count);
            auto right = std::span(sample_right).first(count);
            if (plugin.try_process_realtime(left, right,
                        std::span<const ekphore::PluginMidiEvent>(audio_events)
                                .first(pending_event_count))) {
                admitted_events.fetch_add(
                        pending_event_count, std::memory_order_relaxed);
                pending_event_count = 0;
            } else
                midi_rejected_blocks.fetch_add(1, std::memory_order_relaxed);
            midi_audio_blocks.fetch_add(1, std::memory_order_relaxed);
            float peak{};
            for (std::size_t frame = 0; frame < count; ++frame)
                peak = std::max({ peak, std::abs(left[frame]),
                        std::abs(right[frame]) });
            auto previous = midi_audio_peak.load(std::memory_order_relaxed);
            while (previous < peak &&
                    !midi_audio_peak.compare_exchange_weak(
                            previous, peak, std::memory_order_relaxed)) {}
            if (std::ranges::any_of(left,
                        [](float value) {
                            return std::abs(value) > 1.0e-7F;
                        }) ||
                    std::ranges::any_of(right, [](float value) {
                        return std::abs(value) > 1.0e-7F;
                    }))
                midi_audio_audible.store(true, std::memory_order_relaxed);
            for (std::size_t frame = 0; frame < count; ++frame) {
                interleaved[frame * 2] = sample_left[frame];
                interleaved[frame * 2 + 1] = sample_right[frame];
            }
            if (diagnostics &&
                    !reported_audio.load(std::memory_order_relaxed)) {
                const auto audible = std::ranges::any_of(
                        std::span(interleaved).first(count * 2),
                        [](const float value) {
                            return std::abs(value) > 1.0e-7F;
                        });
                if (audible && !reported_audio.exchange(
                                       true, std::memory_order_relaxed))
                    became_audible = true;
            }
            if (!SDL_PutAudioStreamData(stream, interleaved.data(),
                        static_cast<int>(count * 2 * sizeof(float))))
                has_audio_error.store(true);
            offset += count;
        }
        if (diagnostics) {
            const auto elapsed =
                    std::chrono::steady_clock::now() - callback_started;
            const auto budget = std::chrono::duration<double>(
                    static_cast<double>(frame_count) / 48000.0);
            if ((elapsed > budget || became_audible) &&
                    !audio_reports.push({ frame_count,
                            std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(elapsed)
                                    .count(),
                            became_audible, elapsed > budget }))
                lost_audio_reports.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ekphore::Plugin &plugin;
    mutable std::chrono::steady_clock::duration render_time{};
    mutable std::size_t render_count{};
    std::chrono::steady_clock::duration drag_time{};
    std::size_t drag_count{};
    SDL_AudioStream *audio_stream{};
    ekphore::dsp::BufferedSpscQueue<ekphore::PluginMidiEvent, 1024> events;
    std::array<ekphore::PluginMidiEvent, 1024> audio_events{};
    std::size_t pending_event_count{}; // what audio kept after a rejection
    std::atomic<std::size_t> admitted_events{};
    std::atomic<bool> midi_audio_audible{};
    std::atomic<std::size_t> midi_audio_blocks{}, midi_rejected_blocks{};
    std::atomic<float> midi_audio_peak{};
    std::array<bool, 128> held_notes{};
    ekphore::dsp::BufferedSpscQueue<ekphore::PluginMidiEvent, 1024> midi_events;
    std::jthread midi_worker;
    std::shared_ptr<ArrayDialogReply> array_dialog_reply;
    bool array_operation_active{};
    std::chrono::steady_clock::time_point preview_clock{
        std::chrono::steady_clock::now()
    };
    ekphore::Player::EngineCommandHandle array_dialog_command;
    std::array<float, 1024> sample_left{};
    std::array<float, 1024> sample_right{};
    std::array<float, 2048> interleaved{};
    struct AudioReport {
        std::size_t frames{};
        std::int64_t elapsed_ns{};
        bool became_audible{}, missed_deadline{};
    };
    const bool diagnostics;
    ekphore::dsp::SpscQueue<AudioReport, 64> audio_reports;
    std::atomic<std::size_t> lost_audio_reports{};
    std::atomic<bool> has_audio_error{};
    std::atomic<bool> reported_audio{};
};

std::uint8_t keyboard_note(const SDL_Scancode key) {
    switch (key) {
        // bottom row is C2-C3 like most drum samplers
        case SDL_SCANCODE_Z:
            return 36;
        case SDL_SCANCODE_S:
            return 37;
        case SDL_SCANCODE_X:
            return 38;
        case SDL_SCANCODE_D:
            return 39;
        case SDL_SCANCODE_C:
            return 40;
        case SDL_SCANCODE_V:
            return 41;
        case SDL_SCANCODE_G:
            return 42;
        case SDL_SCANCODE_B:
            return 43;
        case SDL_SCANCODE_H:
            return 44;
        case SDL_SCANCODE_N:
            return 45;
        case SDL_SCANCODE_J:
            return 46;
        case SDL_SCANCODE_M:
            return 47;
        case SDL_SCANCODE_COMMA:
            return 48;
        // upper rows are C4-C5
        case SDL_SCANCODE_A:
            return 60;
        case SDL_SCANCODE_W:
            return 61;
        case SDL_SCANCODE_E:
            return 63;
        case SDL_SCANCODE_R:
            return 64;
        case SDL_SCANCODE_F:
            return 65;
        case SDL_SCANCODE_T:
            return 66;
        case SDL_SCANCODE_Y:
            return 68;
        case SDL_SCANCODE_U:
            return 69;
        case SDL_SCANCODE_I:
            return 70;
        case SDL_SCANCODE_O:
            return 71;
        case SDL_SCANCODE_P:
            return 72;
        default:
            return 0;
    }
}

std::optional<ekphore::UiKey> text_edit_key(
        const SDL_Scancode key, const SDL_Keymod modifiers) {
    switch (key) {
        case SDL_SCANCODE_ESCAPE:
            return ekphore::UiKey::escape;
        case SDL_SCANCODE_BACKSPACE:
            return ekphore::UiKey::backspace;
        case SDL_SCANCODE_TAB:
            return ekphore::UiKey::tab;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            return ekphore::UiKey::enter;
        case SDL_SCANCODE_DELETE:
            return ekphore::UiKey::delete_forward;
        case SDL_SCANCODE_HOME:
            return ekphore::UiKey::home;
        case SDL_SCANCODE_END:
            return ekphore::UiKey::end;
        case SDL_SCANCODE_LEFT:
            return ekphore::UiKey::left;
        case SDL_SCANCODE_RIGHT:
            return ekphore::UiKey::right;
        case SDL_SCANCODE_UP:
            return ekphore::UiKey::up;
        case SDL_SCANCODE_DOWN:
            return ekphore::UiKey::down;
        case SDL_SCANCODE_A:
            if ((modifiers & SDL_KMOD_CTRL) != 0 ||
                    (modifiers & SDL_KMOD_GUI) != 0)
                return ekphore::UiKey::select_all;
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

void usage(const char *executable) {
    std::cerr << "usage: " << executable
              << " [instrument.nki|snapshot.nksn] [--ui-only] "
                 "[--performance-view-script INDEX]\n"
              << "       " << executable << " [--library FOLDER] [--ui-only]\n";
}

std::filesystem::path browser_config_path() {
    if (const auto *override_path = std::getenv("EKPHORE_CONFIG_DIR");
            override_path && *override_path)
        return std::filesystem::u8path(override_path) / "config.ini";
    return ekphore::browser::config_path(
#ifdef _WIN32
            ekphore::browser::Platform::windows,
#elif defined(__APPLE__)
            ekphore::browser::Platform::macos,
#else
            ekphore::browser::Platform::linux_os,
#endif
            [](const char *name) {
                const auto *value = std::getenv(name);
                return value ? std::string(value) : std::string{};
            });
}

std::optional<std::filesystem::path> browse_library(Window &window,
        const std::filesystem::path &config, ekphore::nks::Client &nks_client,
        ekphore::browser::View &view, std::string message = {}) {
    using namespace ekphore::browser;
    view.message = std::move(message);
    ekphore::nks::State surface;
    const auto loading = [&] {
        surface.status = "Busy... Loading";
        nks_client.publish(surface);
    };
    std::string nks_product;
    auto nks_deadline = std::chrono::steady_clock::time_point{};
    try {
        if (const auto settings = read_settings(config)) {
            view.folder = settings->folder;
            view.set_favorites(settings->favorites);
        }
    } catch (const std::exception &e) { view.message = e.what(); }
    struct ScanReply {
        std::mutex mutex;
        std::optional<Catalog> value;
    };
    auto scanned = std::make_shared<ScanReply>();
    std::jthread scanner;
    const auto rescan = [&](bool refresh = false) {
        if (scanner.joinable()) {
            scanner.request_stop();
            scanner.join();
        }
        if (refresh)
            invalidate_catalog(view.folder);
        view.catalog.scanning = true;
        {
            std::scoped_lock lock(scanned->mutex);
            scanned->value.reset();
        }
        scanner = std::jthread(
                [root = view.folder, scanned](std::stop_token stop) {
                    const auto publish = [&](const Catalog &result) {
                        if (stop.stop_requested())
                            return;
                        std::scoped_lock lock(scanned->mutex);
                        scanned->value = result;
                    };
                    try {
                        publish(cached_scan(root, stop, publish));
                    } catch (const std::exception &e) {
                        Catalog result;
                        result.error = e.what();
                        publish(result);
                    }
                });
    };
    if (!view.folder.empty())
        rescan();
    ekphore::UiDocument document;
    document.title = "ekphore — Library";
    document.width = View::width;
    document.height = View::height;
    if (!window.open(document, false))
        throw std::runtime_error("Could not open the library window.");
    std::shared_ptr<ArrayDialogReply> dialog;
    bool key_dialog = false;
    bool running = true, repaint = true;
    bool caret_visible = true;
    auto caret_epoch = SDL_GetTicks();
    while (running) {
        if (nks_client.available() &&
                std::chrono::steady_clock::now() >= nks_deadline) {
            nks_deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(16);
            surface = {};
            surface.name = "ekphore Library";
            surface.identity = view.folder.string();
            surface.selected = view.selection();
            surface.favorites_only = view.favorites_filter();
            surface.product = nks_product;
            surface.product_short_names = view.catalog.short_names;
            for (const auto index : view.visible_entries()) {
                const auto &entry = view.catalog.entries[index];
                surface.sounds.push_back({ entry.path, entry.name,
                        entry.library, view.is_favorite(entry.path),
                        entry.kind == Kind::preset });
            }
            std::optional<std::filesystem::path> load;
            try {
                nks_client.poll(surface, [&](const auto &action) {
                    if (auto requested =
                                    ekphore::nks::apply_action(surface, action))
                        load = std::move(requested);
                    nks_product = surface.product;
                    view.select(surface.selected);
                    repaint = true;
                    if (const auto *trigger =
                                    std::get_if<libnks::BrowserTriggerAction>(
                                            &action);
                            trigger &&
                            (trigger->kind ==
                                            libnks::OdrBrowserTriggerKind::
                                                    toggle_filter_favorite_presets ||
                                    trigger->kind ==
                                            libnks::OdrBrowserTriggerKind::
                                                    reset_filters))
                        view.filter_favorites(surface.favorites_only);
                    if (const auto *decoded =
                                    std::get_if<libnks::BrowserSoundAction>(
                                            &action);
                            decoded &&
                            decoded->kind == libnks::OdrBrowserSoundActionKind::
                                                     toggle_favorite) {
                        if (decoded->value >= 0 &&
                                std::uint64_t(decoded->value) <
                                        surface.sounds.size()) {
                            const auto saved = read_settings(config);
                            auto favorites =
                                    saved ? saved->favorites : view.favorites();
                            const auto &sound =
                                    surface.sounds[std::size_t(decoded->value)];
                            if (sound.favorite)
                                favorites.insert(sound.path.lexically_normal());
                            else
                                favorites.erase(sound.path.lexically_normal());
                            save_config(config, view.folder, favorites);
                            view.set_favorites(std::move(favorites));
                        }
                    }
                });
            } catch (const std::exception &e) {
                view.message = e.what();
                repaint = true;
            }
            if (load) {
                loading();
                window.set_text_input(false);
                scanner.request_stop();
                return load;
            }
        }
        {
            std::optional<Catalog> update;
            {
                std::scoped_lock lock(scanned->mutex);
                update.swap(scanned->value);
            }
            if (update) {
                view.update(std::move(*update));
                repaint = true;
            }
        }
        if (dialog) {
            std::optional<std::filesystem::path> path;
            bool ready{};
            std::string error;
            {
                std::scoped_lock lock(dialog->mutex);
                ready = dialog->ready;
                path = dialog->path;
                error = dialog->error;
            }
            if (ready) {
                dialog.reset();
                view.choosing = false;
                repaint = true;
                view.choosing_key = false;
                if (!error.empty())
                    view.message = "File selection failed: " + error;
                if (path)
                    try {
                        if (key_dialog)
                            ekphore::keys::save(view.folder, *path);
                        else {
                            save_config(config, *path, view.favorites());
                            view.folder = std::filesystem::absolute(*path);
                        }
                        view.invalid_key = false;
                        view.message.clear();
                        view.update({});
                        view.clear_search();
                        rescan();
                    } catch (const std::exception &e) {
                        view.message = e.what();
                        view.invalid_key = key_dialog;
                    }
            }
        }
        const bool caret_phase =
                ((SDL_GetTicks() - caret_epoch) / 500) % 2 == 0;
        if (view.search_focused() && caret_phase != caret_visible)
            repaint = true;
        caret_visible = caret_phase;
        if (repaint) {
            if (!window.present(
                        view.render(window.display_scale(), caret_visible)))
                throw std::runtime_error("Could not draw the library window.");
            repaint = false;
        }
        SDL_Event event{};
        if (!window.wait_event(event, 16))
            continue;
        const auto event_batch_started = std::chrono::steady_clock::now();
        std::size_t event_count{};
        do {
            Result result;
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    repaint = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    if (event.button.button != SDL_BUTTON_LEFT)
                        break;
                    nks_client.focus();
                    const auto [x, y] = window.document_position(
                            event.button.x, event.button.y, document);
                    result = view.click(int(x), int(y));
                    window.set_text_input(view.search_focused());
                    caret_epoch = SDL_GetTicks();
                    repaint = true;
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    view.release();
                    break;
                case SDL_EVENT_MOUSE_MOTION: {
                    const auto [x, y] = window.document_position(
                            event.motion.x, event.motion.y, document);
                    repaint = view.move(int(x), int(y)) || repaint;
                    if (event.motion.state & SDL_BUTTON_LMASK)
                        repaint = view.drag(int(y)) || repaint;
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL: {
                    const auto [x, y] = window.document_position(
                            event.wheel.mouse_x, event.wheel.mouse_y, document);
                    repaint = view.scroll(int(x), int(y),
                                      wheel_scroll_pixels(event.wheel, 54.)) ||
                              repaint;
                    break;
                }
                case SDL_EVENT_TEXT_INPUT:
                    if (view.search_focused() && event.text.text) {
                        view.search(event.text.text);
                        caret_epoch = SDL_GetTicks();
                        repaint = true;
                    }
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (view.search_focused()) {
                        const bool shortcut =
                                (event.key.mod &
                                        (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
                        if (shortcut && event.key.scancode == SDL_SCANCODE_A) {
                            view.select_all();
                            repaint = true;
                            break;
                        }
                        if (shortcut && event.key.scancode == SDL_SCANCODE_V) {
                            if (auto *text = SDL_GetClipboardText()) {
                                view.search(text);
                                SDL_free(text);
                            }
                            caret_epoch = SDL_GetTicks();
                            repaint = true;
                            break;
                        }
                        if (event.key.scancode == SDL_SCANCODE_LEFT ||
                                event.key.scancode == SDL_SCANCODE_RIGHT ||
                                event.key.scancode == SDL_SCANCODE_HOME ||
                                event.key.scancode == SDL_SCANCODE_END) {
                            view.move_cursor(
                                    event.key.scancode == SDL_SCANCODE_LEFT ||
                                                    event.key.scancode ==
                                                            SDL_SCANCODE_HOME
                                            ? -1
                                            : 1,
                                    shortcut ||
                                            event.key.scancode ==
                                                    SDL_SCANCODE_HOME ||
                                            event.key.scancode ==
                                                    SDL_SCANCODE_END);
                            caret_epoch = SDL_GetTicks();
                            repaint = true;
                            break;
                        }
                        if (event.key.scancode == SDL_SCANCODE_DELETE) {
                            view.delete_forward();
                            caret_epoch = SDL_GetTicks();
                            repaint = true;
                            break;
                        }
                    }
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        view.clear_search();
                        repaint = true;
                    } else if (view.search_focused() &&
                               event.key.scancode == SDL_SCANCODE_BACKSPACE) {
                        view.backspace();
                        repaint = true;
                    } else if (event.key.scancode == SDL_SCANCODE_DOWN ||
                               event.key.scancode == SDL_SCANCODE_UP) {
                        result = view.key(
                                event.key.scancode == SDL_SCANCODE_DOWN ? 1
                                                                        : -1);
                        repaint = true;
                    } else if (event.key.scancode == SDL_SCANCODE_RETURN ||
                               event.key.scancode == SDL_SCANCODE_KP_ENTER)
                        result = view.key(0, true);
                    break;
                default:
                    break;
            }
            if (result.action == Action::load) {
                loading();
                window.set_text_input(false);
                scanner.request_stop();
                return result.path;
            }
            if (result.action == Action::favorites_changed) {
                try {
                    save_config(config, view.folder, view.favorites());
                    view.message.clear();
                } catch (const std::exception &e) {
                    auto rollback = view.favorites();
                    if (view.is_favorite(result.path))
                        rollback.erase(result.path.lexically_normal());
                    else
                        rollback.insert(result.path.lexically_normal());
                    view.set_favorites(std::move(rollback));
                    view.message = "Could not save favorites: " +
                                   std::string(e.what());
                }
                repaint = true;
            }
            if (result.action == Action::rescan) {
                view.message.clear();
                rescan(true);
                repaint = true;
            }
            if (result.action == Action::skip_key) {
                ekphore::keys::skip_for_session();
                view.message.clear();
                rescan();
                repaint = true;
            }
            if (result.action == Action::choose_key && !dialog) {
                key_dialog = true;
                view.choosing_key = true;
                view.invalid_key = false;
                view.message.clear();
                dialog = std::make_shared<ArrayDialogReply>();
                window.key_dialog(dialog);
                repaint = true;
            }
            if (result.action == Action::choose_folder && !dialog) {
                key_dialog = false;
                view.choosing = true;
                dialog = std::make_shared<ArrayDialogReply>();
                window.folder_dialog(dialog, view.folder);
                repaint = true;
            }
        } while (running && ++event_count < 256 &&
                 std::chrono::steady_clock::now() - event_batch_started <
                         std::chrono::milliseconds(2) &&
                 window.poll_event(event));
    }
    scanner.request_stop();
    return {};
}

} // namespace

int run_instrument(Window &window, const int argc, char **argv,
        std::shared_ptr<ekphore::nks::Client> keyboard_client = {}) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::optional<std::size_t> performance_view_script;
    bool ui_only = false;
    for (int argument = 2; argument < argc; ++argument) {
        if (std::string_view(argv[argument]) == "--ui-only") {
            ui_only = true;
            continue;
        }
        if (std::string_view(argv[argument]) != "--performance-view-script" ||
                argument + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        try {
            std::size_t consumed{};
            const auto value = std::stoull(argv[++argument], &consumed);
            if (consumed != std::string_view(argv[argument]).size())
                throw std::invalid_argument("index");
            performance_view_script = static_cast<std::size_t>(value);
        } catch (const std::exception &) {
            std::cerr << "Invalid performance-view script index\n";
            return 2;
        }
    }

    ekphore::Plugin plugin(std::move(keyboard_client));
    const auto mode = ui_only ? ekphore::InstrumentLoadMode::ui_preview
                              : ekphore::InstrumentLoadMode::playback;
    ekphore::LoadReport report;
    if (std::getenv("EKPHORE_UI_CAPTURE_PPM")) {
        // headless capture doesn't need a window or audio device
        report = plugin.load(
                std::filesystem::path(argv[1]), performance_view_script, mode);
    } else {
        // window first, loading doesn't run on SDL's thread
        ekphore::UiDocument loading;
        loading.title = "ekphore — Loading instrument";
        loading.width = ekphore::browser::View::width;
        loading.height = ekphore::browser::View::height;
        if (!window.open(loading, false))
            return 1;
        (void)plugin.request_load(
                std::filesystem::path(argv[1]), performance_view_script, mode);
        bool closed = false;
        while (plugin.instrument_loading()) {
            if (!closed)
                (void)window.present(
                        plugin.render_editor(window.display_scale()));
            SDL_Event event{};
            if (window.wait_event(event, 16))
                do {
                    if (event.type == SDL_EVENT_QUIT ||
                            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                        closed = true;
                } while (window.poll_event(event));
        }
        report = plugin.finish_load();
        if (closed)
            return 0;
    }
    if (!report.ok()) {
        std::cerr << "Could not load instrument";
        for (const auto &diagnostic : report.diagnostics)
            std::cerr << "\n  " << diagnostic;
        std::cerr << '\n';
        return 1;
    }
    // a loaded UI can still have missing audio resources, so report warnings
    // even when loading succeeded
    constexpr std::size_t maximum_load_warnings = 20;
    for (std::size_t index = 0;
            index < std::min(report.diagnostics.size(), maximum_load_warnings);
            ++index)
        std::cerr << "Instrument load warning: " << report.diagnostics[index]
                  << '\n';
    if (report.diagnostics.size() > maximum_load_warnings)
        std::cerr << "Instrument load warning: "
                  << report.diagnostics.size() - maximum_load_warnings
                  << " additional diagnostics omitted\n";

    auto document = plugin.editor_document();
    if (ui_only) {
        document.title += " (UI preview - no audio)";
        std::cerr << "UI-only preview: sample headers/PCM skipped; not for "
                     "audio validation.\n";
    }
    document.title += " — F2: Library";
    if (const auto *capture = std::getenv("EKPHORE_UI_CAPTURE_PPM")) {
        // goes through the realtime path but without an audio device
        const bool live_capture =
                !ui_only && std::getenv("EKPHORE_UI_CAPTURE_LIVE") != nullptr;
        std::array<float, 64> capture_left{}, capture_right{};
        if (live_capture)
            plugin.activate(48000, 64);
        const auto process_capture = [&] {
            if (live_capture)
                plugin.process_realtime(capture_left, capture_right);
        };
        if (const auto *click = std::getenv("EKPHORE_UI_CAPTURE_CLICK")) {
            // render once or there's nothing to click on
            (void)plugin.render_editor(1.0F);
            std::int32_t x{};
            std::int32_t y{};
            const std::string_view coordinates(click);
            if (const auto comma = coordinates.find(',');
                    comma != std::string_view::npos) {
                const auto x_text = coordinates.substr(0, comma);
                const auto y_text = coordinates.substr(comma + 1);
                if (std::from_chars(
                            x_text.data(), x_text.data() + x_text.size(), x)
                                        .ec == std::errc{} &&
                        std::from_chars(
                                y_text.data(), y_text.data() + y_text.size(), y)
                                        .ec == std::errc{}) {
                    (void)plugin.pointer_down(x, y);
                    process_capture();
                    plugin.pointer_up();
                }
            }
        }
        if (const auto *drag = std::getenv("EKPHORE_UI_CAPTURE_DRAG")) {
            // start-x,start-y,end-x,end-y in logical coordinates
            std::array<std::int32_t, 4> points{};
            std::string_view remaining(drag);
            bool valid = true;
            for (std::size_t index = 0; index < points.size(); ++index) {
                const auto comma = remaining.find(',');
                const auto part = remaining.substr(0, comma);
                const auto parsed = std::from_chars(
                        part.data(), part.data() + part.size(), points[index]);
                valid &= parsed.ec == std::errc{} &&
                         parsed.ptr == part.data() + part.size();
                if (index + 1 < points.size()) {
                    if (comma == std::string_view::npos) {
                        valid = false;
                        break;
                    }
                    remaining.remove_prefix(comma + 1);
                } else
                    valid &= comma == std::string_view::npos;
            }
            if (valid) {
                (void)plugin.render_editor(1.0F);
                (void)plugin.pointer_down(points[0], points[1]);
                for (int step = 1; step <= 10; ++step) {
                    (void)plugin.pointer_drag(
                            points[0] + (points[2] - points[0]) * step / 10,
                            points[1] + (points[3] - points[1]) * step / 10);
                    process_capture();
                    (void)plugin.render_editor(1.0F);
                }
                plugin.pointer_up();
            }
        }
        // let the UI callbacks settle, no samples and no real waiting
        if (ui_only) {
            if (const auto *advance =
                            std::getenv("EKPHORE_UI_CAPTURE_ADVANCE_FRAMES")) {
                std::size_t frames{};
                const std::string_view count(advance);
                if (std::from_chars(
                            count.data(), count.data() + count.size(), frames)
                                .ec == std::errc{})
                    (void)plugin.advance_ui_preview_clock(frames);
            }
        }
        if (std::getenv("EKPHORE_UI_DIAGNOSTICS_ALL")) {
            const auto capture_document = plugin.editor_document();
            std::cerr << "capture-native-roots="
                      << capture_document.native_ui_component_roots.size()
                      << '\n';
            for (const auto &diagnostic :
                    capture_document.native_ui_diagnostics)
                std::cerr << "capture-native-diagnostic=" << diagnostic << '\n';
            for (const auto &control : capture_document.controls) {
                if (control.kind != ekphore::UiControlKind::menu)
                    continue;
                std::cerr << "capture-menu=" << control.name << " value=";
                const auto *value = std::get_if<std::int64_t>(&control.value);
                if (value)
                    std::cerr << *value;
                else
                    std::cerr << '?';
                const auto selected =
                        value ? std::ranges::find(control.menu_entries, *value,
                                        &std::pair<std::int64_t,
                                                std::string>::first)
                              : control.menu_entries.end();
                std::cerr << " entries=" << control.menu_entries.size()
                          << " selected="
                          << (selected == control.menu_entries.end()
                                             ? std::string{ "?" }
                                             : selected->second)
                          << '\n';
            }
        }
        process_capture();
        const auto frame = plugin.render_editor();
        if (live_capture)
            plugin.deactivate();
        std::ofstream output(capture, std::ios::binary | std::ios::trunc);
        output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";
        for (std::size_t offset = 0; offset < frame.rgba.size(); offset += 4)
            output.write(
                    reinterpret_cast<const char *>(frame.rgba.data() + offset),
                    3);
        return output ? 0 : 1;
    }
    if (std::getenv("EKPHORE_UI_DIAGNOSTICS")) {
        const auto pictured = std::ranges::count_if(document.controls,
                [](const auto &control) { return !control.picture.empty(); });
        std::cerr << "UI " << document.width << 'x' << document.height
                  << " controls=" << document.controls.size()
                  << " pictured=" << pictured
                  << " wallpaper=" << document.background_picture << '\n';
        for (const auto &control : document.controls) {
            std::string lower_name = control.name;
            std::ranges::transform(lower_name, lower_name.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
            if (std::getenv("EKPHORE_UI_DIAGNOSTICS_ALL") ||
                    control.kind == ekphore::UiControlKind::text_edit ||
                    lower_name.find("tune") != std::string::npos ||
                    lower_name.find("global_fx_power") != std::string::npos ||
                    lower_name == "$tabs") {
                std::cerr << "UI control " << control.name << '='
                          << control.text << " bounds=" << control.bounds.x
                          << ',' << control.bounds.y << ','
                          << control.bounds.width << ','
                          << control.bounds.height
                          << " align=" << control.text_alignment
                          << " font_type=" << control.font_type
                          << " font_name=" << control.font_name
                          << " picture=" << control.picture
                          << " parent=" << control.parent_name
                          << " id=" << control.ksp_id
                          << " state=" << control.picture_state << " value=";
                if (const auto *value =
                                std::get_if<std::int64_t>(&control.value))
                    std::cerr << *value;
                else
                    std::cerr << '?';
                if (control.kind == ekphore::UiControlKind::menu) {
                    std::cerr << " entries=" << control.menu_entries.size();
                    if (const auto *value =
                                    std::get_if<std::int64_t>(&control.value)) {
                        const auto selected = std::ranges::find(
                                control.menu_entries, *value,
                                &std::pair<std::int64_t, std::string>::first);
                        if (selected != control.menu_entries.end())
                            std::cerr << " selected=" << selected->second;
                    }
                }
                std::cerr << '\n';
            }
        }
    }
    if (!window.open(document, !ui_only))
        return 1;
    StandaloneRuntime runtime(plugin);
    if (!ui_only && !runtime.start())
        return 1;
    if (!window.present(runtime.render_editor(window.display_scale())))
        return 1;

    bool running = true;
    bool return_to_library = false;
    std::vector<std::filesystem::path> dropped_files;
    float drop_x{};
    float drop_y{};
    constexpr auto frame_period = std::chrono::microseconds{ 16667 };
    auto next_frame = std::chrono::steady_clock::now();
    bool was_instrument_loading = false;
    while (running) {
        const bool array_repaint = runtime.service_array_dialog(window);
        SDL_Event event{};
        const bool preparing = plugin.instrument_loading();
        const bool loading_transition = preparing != was_instrument_loading;
        was_instrument_loading = preparing;
        const bool loading = document.loading_progress.has_value() || preparing;
        const auto before_wait = std::chrono::steady_clock::now();
        const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        next_frame - before_wait)
                        .count();
        const bool received_event = window.wait_event(event,
                static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 16)));
        const bool frame_due = std::chrono::steady_clock::now() >= next_frame;
        // audio callbacks can change animation without input; keep polling
        // even when the mouse is still
        bool repaint = (ui_only && runtime.advance_preview_clock()) ||
                       (frame_due && runtime.menu_animating()) ||
                       (!ui_only && frame_due) || array_repaint ||
                       (loading && frame_due) || loading_transition;
        // input first, but capped or a busy queue means we never repaint
        constexpr std::size_t maximum_events_per_frame = 16;
        constexpr auto maximum_event_batch_time =
                std::chrono::milliseconds{ 2 };
        const auto event_batch_started = std::chrono::steady_clock::now();
        std::size_t event_count{};
        while (received_event) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                    repaint = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    // focus loss may swallow key-up events; release held notes
                    // now instead of leaving them sounding
                    runtime.release_held_notes();
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    const auto [x, y] = window.document_position(
                            event.button.x, event.button.y, document);
                    repaint = runtime.pointer_down(static_cast<std::int32_t>(x),
                                      static_cast<std::int32_t>(y)) ||
                              repaint;
                    window.set_text_input(runtime.text_editing_active());
                    break;
                }
                case SDL_EVENT_MOUSE_MOTION: {
                    const auto [x, y] = window.document_position(
                            event.motion.x, event.motion.y, document);
                    const auto changed =
                            (event.motion.state & SDL_BUTTON_LMASK) != 0
                                    ? runtime.pointer_drag(
                                              static_cast<std::int32_t>(x),
                                              static_cast<std::int32_t>(y))
                                    : runtime.pointer_move(
                                              static_cast<std::int32_t>(x),
                                              static_cast<std::int32_t>(y));
                    repaint = changed || repaint;
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL: {
                    const auto [x, y] = window.document_position(
                            event.wheel.mouse_x, event.wheel.mouse_y, document);
                    repaint = runtime.pointer_scroll(static_cast<int>(x),
                                      static_cast<int>(y),
                                      wheel_scroll_pixels(event.wheel, 72.0)) ||
                              repaint;
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    runtime.pointer_up();
                    // release can finalize script values/colors even if the
                    // last motion changed nothing, so repaint the final state
                    repaint = true;
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    if (event.text.text)
                        repaint =
                                runtime.text_input(event.text.text) || repaint;
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP: {
                    if (event.type == SDL_EVENT_KEY_DOWN &&
                            event.key.scancode == SDL_SCANCODE_F2) {
                        runtime.release_held_notes();
                        return_to_library = true;
                        running = false;
                        break;
                    }
                    if (runtime.text_editing_active() || runtime.menu_open()) {
                        if (event.type == SDL_EVENT_KEY_DOWN &&
                                !event.key.repeat)
                            if (const auto key = text_edit_key(
                                        event.key.scancode, event.key.mod))
                                repaint =
                                        runtime.key_down(*key,
                                                (event.key.mod &
                                                        SDL_KMOD_SHIFT) != 0,
                                                (event.key.mod &
                                                        (SDL_KMOD_CTRL |
                                                                SDL_KMOD_GUI)) !=
                                                        0) ||
                                        repaint;
                        window.set_text_input(runtime.text_editing_active());
                        break;
                    }
                    const auto note = keyboard_note(event.key.scancode);
                    if (note != 0 && (event.type == SDL_EVENT_KEY_UP ||
                                             !event.key.repeat))
                        runtime.note(note, event.type == SDL_EVENT_KEY_DOWN);
                    break;
                }
                case SDL_EVENT_DROP_BEGIN:
                    dropped_files.clear();
                    break;
                case SDL_EVENT_DROP_FILE:
                    drop_x = event.drop.x;
                    drop_y = event.drop.y;
                    if (event.drop.data)
                        dropped_files.emplace_back(event.drop.data);
                    break;
                case SDL_EVENT_DROP_COMPLETE: {
                    drop_x = event.drop.x;
                    drop_y = event.drop.y;
                    const auto [x, y] =
                            window.document_position(drop_x, drop_y, document);
                    const auto keys = SDL_GetModState();
                    std::uint32_t modifiers{};
                    if ((keys & SDL_KMOD_SHIFT) != 0)
                        modifiers |= UINT32_C(1) << 28;
                    if ((keys & SDL_KMOD_CTRL) != 0)
                        modifiers |= UINT32_C(1) << 29;
                    if ((keys & SDL_KMOD_ALT) != 0)
                        modifiers |= UINT32_C(1) << 31;
                    repaint = runtime.drop_files(static_cast<std::int32_t>(x),
                                      static_cast<std::int32_t>(y),
                                      dropped_files, modifiers) ||
                              repaint;
                    dropped_files.clear();
                    break;
                }
                default:
                    break;
            }
            ++event_count;
            if (!running || event_count >= maximum_events_per_frame ||
                    std::chrono::steady_clock::now() - event_batch_started >=
                            maximum_event_batch_time ||
                    !window.poll_event(event))
                break;
        }
        if (repaint) {
            const auto frame_started = std::chrono::steady_clock::now();
            document = runtime.editor_document();
            (void)window.present(runtime.render_editor(window.display_scale()));
            const auto now = std::chrono::steady_clock::now();
            next_frame = frame_started + frame_period;
            if (next_frame < now)
                next_frame = now;
        }
        if (runtime.audio_failed()) {
            std::cerr << "Audio stream write failed: " << SDL_GetError()
                      << '\n';
            running = false;
        }
    }
    return return_to_library ? 42 : 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    attach_terminal();
#endif
    // nksd first, only standalone opens USB itself and the lock keeps us from
    // racing a daemon that's starting up
    auto keyboard_client = std::make_shared<ekphore::nks::Client>(
            ekphore::nks::runtime_directory(),
            libnks::OdrInstanceType::standalone);
    Window window; // one window for everything
    bool ui_only = false;
    std::optional<std::filesystem::path> initial_folder;
    bool direct = argc > 1 && !std::string_view(argv[1]).starts_with("--");
    if (direct && !ekphore::keys::available(std::filesystem::u8path(argv[1]))) {
        try {
            const auto path =
                    std::filesystem::absolute(std::filesystem::u8path(argv[1]));
            if (ekphore::keys::required(path)) {
                auto folder = path.parent_path();
                if (const auto settings = ekphore::browser::read_settings(
                            browser_config_path());
                        settings &&
                        !path.lexically_relative(settings->folder).empty() &&
                        *path.lexically_relative(settings->folder).begin() !=
                                "..")
                    folder = settings->folder;
                initial_folder = folder;
                direct = false;
                for (int i = 2; i < argc; ++i)
                    if (std::string_view(argv[i]) == "--ui-only")
                        ui_only = true;
            }
        } catch (const std::exception &) {}
    }
    if (direct) {
        const auto code = run_instrument(window, argc, argv, keyboard_client);
        if (code != 42)
            return code;
        for (int i = 2; i < argc; ++i)
            if (std::string_view(argv[i]) == "--ui-only")
                ui_only = true;
    } else if (!initial_folder) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view argument = argv[i];
            if (argument == "--ui-only")
                ui_only = true;
            else if (argument == "--library" && i + 1 < argc)
                initial_folder = std::filesystem::u8path(argv[++i]);
            else {
                usage(argv[0]);
                return argument == "--help" ? 0 : 2;
            }
        }
    }
    try {
        const auto config = browser_config_path();
        if (initial_folder)
            ekphore::browser::save_config(config, *initial_folder);
        std::string message;
        ekphore::browser::View browser_view;
        for (;;) {
            const auto selected = browse_library(
                    window, config, *keyboard_client, browser_view, message);
            if (!selected)
                return 0;
            const auto encoded = selected->u8string();
            std::string path(reinterpret_cast<const char *>(encoded.data()),
                    encoded.size());
            char preview[] = "--ui-only";
            char *arguments[] = { argv[0], path.data(), preview };
            const auto code = run_instrument(
                    window, ui_only ? 3 : 2, arguments, keyboard_client);
            if (code == 0)
                return 0;
            message = code == 42 ? ""
                                 : "This sound could not be opened. Try "
                                   "another file; details are in the terminal.";
        }
    } catch (const std::exception &e) {
        std::cerr << "Library browser: " << e.what() << '\n';
        return 1;
    }
}
