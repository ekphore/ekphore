// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../dsp/profile.h"
#include "../player/scheduling.h"
#include "../realtime/queues/retained_spsc_queue.h"
#include "../ui/browser.h"
#include "../ui/native/binding.h"
#include "../ui/snapshot.h"
#include "../ui/trace.h"
#include "classic_picture_metadata.h"
#include "input_publication.h"
#include "parameter_publication.h"
#include <ekphore/plugin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
#include <unordered_map>

#include <png.h>
#include <tiffio.h>
#include <webp/decode.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvg.h>
#include <nanosvgrast.h>

namespace ekphore {
struct PluginLoadJob {
    UiDocument document;
    LoadReport report;
    std::atomic<bool> done{};
    std::chrono::steady_clock::time_point started =
            std::chrono::steady_clock::now();
    std::jthread worker;
};

bool Plugin::instrument_loading() const {
    return nks_loading.load(std::memory_order_acquire) ||
           (load_job && !load_job->done.load(std::memory_order_acquire));
}

bool Plugin::instrument_loaded() const {
    std::scoped_lock lock(engine_owner);
    return has_instrument;
}

bool Plugin::request_load(const std::filesystem::path &path,
        const std::optional<std::size_t> script,
        const InstrumentLoadMode mode) {
    if (instrument_loading())
        return false;
    load_job.reset();
    auto job = std::make_unique<PluginLoadJob>();
    const auto current = editor_document();
    job->document.width = std::max(current.width, 1);
    job->document.height = std::max(current.height, 1);
    job->document.title = "ekphore — Loading instrument";
    job->document.background = { 29, 26, 32, 255 };
    job->document.loading_instrument = path.stem().string();
    if (job->document.loading_instrument.empty())
        job->document.loading_instrument = "Instrument";
    auto *pending = job.get();
    load_job = std::move(job);
    try {
        pending->worker = std::jthread([this, pending, path, script, mode] {
            try {
                pending->report = load(path, script, mode);
            } catch (const std::exception &e) {
                pending->report.diagnostics.push_back(e.what());
            } catch (...) {
                pending->report.diagnostics.push_back(
                        "Instrument loading failed.");
            }
            if (!pending->report.ok()) {
                std::scoped_lock lock(ui_mutex);
                instrument_load_error =
                        pending->report.diagnostics.empty()
                                ? "Could not load that instrument."
                                : pending->report.diagnostics.front();
            }
            pending->done.store(true, std::memory_order_release);
        });
    } catch (...) {
        load_job.reset();
        throw;
    }
    return true;
}

LoadReport Plugin::finish_load() {
    if (!load_job)
        return {};
    if (load_job->worker.joinable())
        load_job->worker.join();
    auto report = std::move(load_job->report);
    load_job.reset();
    return report;
}

// audio can still be using a payload until release, the worker frees the old
// slots. lock engine_owner before draining
struct PluginUiCommands {
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    struct Command {
        Player::ScriptControlCallbackTarget target;
        libksp::Value value;
        bool notify{};
        std::optional<Player::NativeUiWriteBatch> native;
        std::optional<std::pair<std::size_t, std::array<double, 2>>> xy;
        std::optional<std::filesystem::path> file_selection;
        std::optional<UiMouseAreaEvent> mouse;
        std::optional<Player::PerformanceViewSelection> performance_view;
        std::optional<std::uint64_t> reset_generation;
        std::optional<std::pair<std::vector<std::int32_t>, std::int32_t>>
                waveform;
        std::uint64_t sequence{};
        int xy_mouse_event_type{};
    };
    static constexpr std::size_t capacity = 256;
    dsp::BufferedRetainedSpscQueue<Command, capacity> commands;
    std::atomic<bool> native_readback_dirty{};
    // UI only, the worker frees ring slots without looking at these
    std::atomic<std::size_t> pending_count{};
    std::mutex producer_mutex;
    std::function<void()> notify;
    std::deque<Command> previews;
    std::uint64_t next_sequence{ 1 }; // UI only
    std::atomic<std::uint64_t> completed_sequence{};
    bool submit(Command command) {
        std::scoped_lock lock(producer_mutex);
        command.sequence = next_sequence;
        previews.push_back(command);
        // count BEFORE emplace, audio can grab it right after
        ++pending_count;
        bool accepted{};
        try {
            accepted = commands.emplace(std::move(command));
        } catch (...) {
            --pending_count;
            previews.pop_back();
            throw;
        }
        if (accepted) {
            ++next_sequence;
            native_readback_dirty = true;
            if (notify)
                notify();
        } else {
            --pending_count;
            previews.pop_back();
        }
        return accepted;
    }
    // use the watermark from the snapshot, a command that finished after it
    // still has to show up
    void project(PlayerUiReadState &state) {
        // the worker never touches previews, no lock needed
        state.submitted_ui_sequence = next_sequence - 1;
        while (!previews.empty() &&
                previews.front().sequence <= state.completed_ui_sequence)
            previews.pop_front();
        for (const auto &command : previews) {
            if (command.native) {
                if (command.native->generation != state.generation)
                    continue;
                for (const auto &write : command.native->writes) {
                    for (auto &c : state.document.controls) {
                        if (detail::native_ui_readback_name(c.name) !=
                                detail::native_ui_readback_name(write.name))
                            continue;
                        if (auto value = detail::native_ui_project_write(
                                    c, write, c.value))
                            c.value = std::move(*value);
                        break;
                    }
                }
                continue;
            }
            if (command.target.generation != state.generation ||
                    command.mouse || command.file_selection ||
                    command.performance_view || command.reset_generation)
                continue;
            for (auto &c : state.document.controls) {
                if (c.script_slot != command.target.script_slot ||
                        detail::native_ui_readback_name(c.name) !=
                                detail::native_ui_readback_name(
                                        command.target.control))
                    continue;
                if (command.xy) {
                    if (auto values = std::get_if<std::vector<double>>(
                                &c.value)) {
                        const auto first = command.xy->first;
                        if (first < values->size()) {
                            c.xy_active_index = static_cast<int>(first / 2);
                            (*values)[first] = command.xy->second[0];
                            if (first + 1 < values->size())
                                (*values)[first + 1] = command.xy->second[1];
                        }
                    }
                } else if (command.waveform) {
                    c.waveform_table = command.waveform->first;
                    c.value = std::int64_t{ command.waveform->second };
                } else
                    c.value = command.value;
                break;
            }
        }
        // the menu selection follows the pending value too
        for (auto &c : state.document.controls) {
            if (c.kind != UiControlKind::menu)
                continue;
            const auto *selected = std::get_if<std::int64_t>(&c.value);
            if (!selected)
                continue;
            const auto entry = std::ranges::find(c.menu_entries, *selected,
                    &std::pair<std::int64_t, std::string>::first);
            if (entry != c.menu_entries.end())
                c.menu_selected_index = static_cast<std::int32_t>(
                        entry - c.menu_entries.begin());
        }
    }
    void pump(const std::function<void()> &before = {}) {
        std::scoped_lock lock(producer_mutex);
        if (before)
            before();
        commands.flush();
    }
    // overflow counts too, don't wait for reclaim to know if everything went
    // through
    std::size_t accepted_count() const noexcept { return pending_count; }
};
struct PluginEventScratch {
    dsp::EventOrder<PluginMidiEvent> midi;
    dsp::EventOrder<PluginParameterEvent> parameters;
    PluginEventScratch() {
        // bigger batches grow, they don't drop events
        midi.prepare(1024);
        parameters.prepare(1024);
    }
};
struct PluginUiAssetCache {
    // set once BEFORE the UI sees it, and it must NOT point into Player
    Player::UiAssetReader reader =
            [](std::string_view) -> std::optional<std::vector<std::byte>> {
        return std::nullopt;
    };
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const UiImage>> images;
    std::unordered_map<std::string,
            std::shared_ptr<const std::vector<std::byte>>>
            data;
};

// runs even with no editor or audio, lock engine_owner before touching Player
// and only use the copied stuff after unlocking
struct PluginCommandService {
    struct PreparedResult {
        Player::EngineCommandHandle command;
        std::unique_ptr<Player::IrPreparation> preparation;
        std::unique_ptr<Player::SamplePreparation> sample;
        std::unique_ptr<Player::EffectPreparation> effect;
        std::unique_ptr<Player::ModelPreparation> model;
        std::unique_ptr<Player::ArrayPreparation> array;
        Player::EngineCommandRetirement retirement;
        PreparedResult(Player::EngineCommandHandle handle,
                std::unique_ptr<Player::IrPreparation> job)
            : command(handle), preparation(std::move(job)) {}
        PreparedResult(Player::EngineCommandHandle handle,
                std::unique_ptr<Player::SamplePreparation> job)
            : command(handle), sample(std::move(job)) {}
        PreparedResult(Player::EngineCommandHandle handle,
                std::unique_ptr<Player::EffectPreparation> job)
            : command(handle), effect(std::move(job)) {}
        PreparedResult(Player::EngineCommandHandle handle,
                std::unique_ptr<Player::ModelPreparation> job)
            : command(handle), model(std::move(job)) {}
        PreparedResult(Player::EngineCommandHandle handle,
                std::unique_ptr<Player::ArrayPreparation> job)
            : command(handle), array(std::move(job)) {}
    };
    // one at a time or a later command can overtake it. audio must NOT free
    // what's in here, it only marks it consumed
    dsp::RetainedSpscQueue<std::unique_ptr<PreparedResult>, 1> prepared_results;
    std::mutex owner_mutex;
    Plugin *owner;
    std::atomic<bool> failed{};
    std::mutex sleep_mutex;
    std::condition_variable_any wake;
    std::atomic<bool> requested{};
    std::mutex processing_request_mutex;
    std::function<void()> processing_request;
    std::shared_ptr<nks::Client> nks_client;
    nks::State nks_surface;
    std::chrono::steady_clock::time_point nks_deadline{};
    std::jthread thread;

    explicit PluginCommandService(
            Plugin &plugin, std::shared_ptr<nks::Client> client = {})
        : owner(&plugin), nks_client(client ? std::move(client)
                                            : std::make_shared<nks::Client>()),
          thread([this](std::stop_token stop) {
              try {
                  nks_client->restore_browser(nks_surface);
                  while (!stop.stop_requested()) {
                      bool notified{};
                      {
                          std::unique_lock lock(sleep_mutex);
                          wake.wait_for(lock, stop,
                                  std::chrono::milliseconds{ 10 },
                                  [this, &notified] {
                                      notified = requested.exchange(false);
                                      return notified;
                                  });
                      }
                      if (!stop.stop_requested()) {
                          if (notified)
                              request_processing();
                          service_batch(stop);
                      }
                  }
              } catch (const std::exception &error) {
                  failed.store(true, std::memory_order_release);
                  std::fprintf(stderr, "ekphore command service: %s\n",
                          error.what());
              } catch (...) {
                  failed.store(true, std::memory_order_release);
                  std::fprintf(
                          stderr, "ekphore command service: unknown failure\n");
              }
          }) {}

    ~PluginCommandService() {
        thread.request_stop();
        wake.notify_all();
        thread.join(); // NEVER with the engine lock held
    }

    void request_processing() {
        std::function<void()> request;
        {
            std::scoped_lock lock(processing_request_mutex);
            request = processing_request;
        }
        if (request)
            request(); // host code, so no locks held
    }

    void deliver_prepared_results(Player &player) {
        if (!prepared_results.available())
            return;
        struct Release {
            decltype(prepared_results) &queue;
            ~Release() { queue.release(1); }
        } release{ prepared_results };
        auto &result = *prepared_results.at(0);
        const bool committed =
                result.sample   ? player.commit_sample_command(*result.sample)
                : result.array  ? player.commit_array_command(*result.array)
                : result.effect ? player.commit_effect_command(*result.effect)
                : result.model  ? player.commit_model_command(*result.model)
                                : player.commit_ir_command(*result.preparation);
        if (committed)
            (void)player.dispatch_engine_command_completion(
                    result.command, result.retirement);
        // rejected jobs stay pending and get captured again
    }

    void service_batch(std::stop_token stop) {
        // only talk to the bridge if our host is still alive
        if (std::chrono::steady_clock::now() >= nks_deadline) {
            nks_deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(16);
            std::scoped_lock binding(owner_mutex);
            try {
                if (nks_client->available()) {
                    auto snapshot = owner->nks_state();
                    nks_surface.name = std::move(snapshot.name);
                    nks_surface.preset = std::move(snapshot.preset);
                    nks_surface.identity = std::move(snapshot.identity);
                    nks_surface.pages = std::move(snapshot.pages);
                    nks_surface.artwork_identity =
                            std::move(snapshot.artwork_identity);
                    nks_surface.lightguide = std::move(snapshot.lightguide);
                    nks_surface.instrument_loaded = snapshot.instrument_loaded;
                    if (snapshot.status == "Busy... Loading")
                        nks_surface.status = snapshot.status;
                    else if (nks_surface.status == "Busy... Loading")
                        nks_surface.status.clear();
                    if (owner->nks_focus_requested.exchange(false))
                        nks_client->focus();
                    nks_client->discover(nks_surface);
                    nks_client->poll(nks_surface, [&](const nks::Action
                                                                  &action) {
                        const auto selected = nks::apply_action(
                                nks_surface, action, [&](auto id, auto value) {
                                    return owner->set_normalized_parameter(
                                            id, value);
                                });
                        // toggling one favorite must not wipe the others
                        if (const auto *decoded =
                                        std::get_if<libnks::BrowserSoundAction>(
                                                &action);
                                decoded &&
                                decoded->kind ==
                                        libnks::OdrBrowserSoundActionKind::
                                                toggle_favorite) {
                            if (auto settings = browser::read_settings(
                                        nks::config_file());
                                    settings && decoded->value >= 0 &&
                                    std::uint64_t(decoded->value) <
                                            nks_surface.sounds.size()) {
                                const auto &sound =
                                        nks_surface.sounds[std::size_t(
                                                decoded->value)];
                                if (sound.favorite)
                                    settings->favorites.insert(
                                            sound.path.lexically_normal());
                                else
                                    settings->favorites.erase(
                                            sound.path.lexically_normal());
                                browser::save_config(nks::config_file(),
                                        settings->folder, settings->favorites);
                            }
                        }
                        if (selected) {
                            nks_surface.status = "Busy... Loading";
                            nks_client->publish(nks_surface);
                            // replacement_mutex keeps this from racing a state
                            // restore
                            const auto report = owner->load(*selected);
                            nks_surface.status =
                                    report.ok() ? ""
                                    : report.diagnostics.empty()
                                            ? "Instrument loading failed"
                                            : report.diagnostics.front();
                            auto loaded = owner->nks_state();
                            nks_surface.name = std::move(loaded.name);
                            nks_surface.preset = std::move(loaded.preset);
                            nks_surface.identity = std::move(loaded.identity);
                            nks_surface.pages = std::move(loaded.pages);
                            nks_surface.artwork_identity =
                                    std::move(loaded.artwork_identity);
                            nks_surface.lightguide =
                                    std::move(loaded.lightguide);
                            nks_surface.instrument_loaded =
                                    loaded.instrument_loaded;
                        }
                    });
                }
            } catch (const std::exception &e) {
                nks_surface.status = e.what();
                std::fprintf(stderr, "ekphore NKS: %s\n", e.what());
            }
        }
        prepared_results.reclaim(); // never with the engine lock
        std::vector<Player::EngineCommandHandle> batch;
        std::shared_ptr<const UiDocument> resources;
        std::shared_ptr<PluginInputPublication> resource_generation;
        {
            std::scoped_lock binding(owner_mutex);
            PluginUiCommands *ui_queue{};
            std::function<void()> before_maintenance;
            {
                std::scoped_lock engine(owner->engine_owner);
                ui_queue = owner->ui_commands.get();
                before_maintenance = owner->before_ui_queue_maintenance;
            }
            // owner_mutex keeps this alive, no need for the engine lock
            if (ui_queue)
                ui_queue->pump(before_maintenance);
            std::scoped_lock engine(owner->engine_owner);
            if (!owner->audio_processing_recent()) {
                owner->drain_ui_controls(false);
                deliver_prepared_results(owner->instrument_player);
                owner->publish_parameters();
            }
            if (owner->input_publication)
                owner->input_publication->reclaim();
            if (!owner->has_instrument)
                return;
            // do it here so audio never takes the sample lock
            if (owner->editor_resources_dirty.exchange(false)) {
                resources = std::make_shared<const UiDocument>(
                        owner->instrument_player.capture_ui_document());
                resource_generation = owner->input_publication;
            }
            // the last result isn't committed yet, don't prepare it again or
            // let a later one overtake it
            if (!prepared_results.available())
                batch = owner->instrument_player.snapshot_engine_commands();
        }
        if (resources) {
            std::scoped_lock binding(owner_mutex);
            std::scoped_lock ui(owner->ui_mutex);
            if (resource_generation == owner->input_publication)
                owner->editor_resources = std::move(resources);
        }
        // callbacks can add commands but not to this batch, or we never stop
        for (const auto command : batch) {
            if (stop.stop_requested())
                return;
            Player::EngineCommandRetirement retirement;
            std::unique_ptr<Player::IrPreparation> preparation;
            std::unique_ptr<Player::SamplePreparation> sample_preparation;
            std::unique_ptr<Player::EffectPreparation> effect_preparation;
            std::unique_ptr<Player::ModelPreparation> model_preparation;
            std::unique_ptr<Player::ArrayPreparation> array_preparation;
            std::function<void()> before_prepare;
            std::function<void()> after_publish;
            {
                std::scoped_lock binding(owner_mutex);
                std::scoped_lock engine(owner->engine_owner);
                owner->consume_host_tempo();
                if (owner->instrument_player
                                .array_command_waiting_for_selection(command))
                    continue;
                array_preparation =
                        owner->instrument_player.snapshot_array_command(
                                command);
                preparation =
                        owner->instrument_player.snapshot_ir_command(command);
                if (!preparation)
                    sample_preparation =
                            owner->instrument_player.snapshot_sample_command(
                                    command);
                if (!preparation && !sample_preparation)
                    effect_preparation =
                            owner->instrument_player.snapshot_effect_command(
                                    command);
                if (!preparation && !sample_preparation && !effect_preparation)
                    model_preparation =
                            owner->instrument_player.snapshot_model_command(
                                    command);
                if (preparation || sample_preparation || effect_preparation ||
                        model_preparation) {
                    before_prepare = owner->before_command_prepare;
                    after_publish = owner->after_command_result_publish;
                }
                if (!preparation && !sample_preparation &&
                        !effect_preparation && !model_preparation &&
                        !array_preparation) {
                    // these touch Player directly, keep the lock
                    (void)owner->instrument_player.execute_engine_command(
                            command);
                    (void)owner->instrument_player
                            .dispatch_engine_command_completion(
                                    command, retirement);
                    owner->publish_parameters();
                }
            }
            if (array_preparation) {
                (void)array_preparation->prepare();
                if (stop.stop_requested())
                    return;
                auto result = std::make_unique<PreparedResult>(
                        command, std::move(array_preparation));
                if (!prepared_results.emplace(std::move(result)))
                    throw std::logic_error(
                            "array result mailbox still occupied");
                request_processing();
                return;
            }
            if (model_preparation) {
                if (before_prepare)
                    before_prepare();
                (void)model_preparation->prepare();
                if (stop.stop_requested())
                    return;
                auto result = std::make_unique<PreparedResult>(
                        command, std::move(model_preparation));
                if (!prepared_results.emplace(std::move(result)))
                    throw std::logic_error(
                            "model result mailbox still occupied");
                request_processing();
                if (after_publish)
                    after_publish();
                return;
            }
            if (effect_preparation) {
                if (before_prepare)
                    before_prepare();
                (void)effect_preparation->prepare();
                if (stop.stop_requested())
                    return;
                auto result = std::make_unique<PreparedResult>(
                        command, std::move(effect_preparation));
                if (!prepared_results.emplace(std::move(result)))
                    throw std::logic_error(
                            "effect result mailbox still occupied");
                request_processing();
                if (after_publish)
                    after_publish();
                return;
            }
            if (sample_preparation) {
                if (before_prepare)
                    before_prepare();
                (void)sample_preparation->prepare();
                if (stop.stop_requested())
                    return;
                {
                    std::scoped_lock binding(owner_mutex);
                    std::scoped_lock engine(owner->engine_owner);
                    if (!owner->instrument_player.stage_sample_command(
                                *sample_preparation))
                        return;
                }
                (void)sample_preparation->publish(); // no lock needed
                if (stop.stop_requested())
                    return;
                auto result = std::make_unique<PreparedResult>(
                        command, std::move(sample_preparation));
                if (!prepared_results.emplace(std::move(result)))
                    throw std::logic_error(
                            "sample result mailbox still occupied");
                request_processing();
                if (after_publish)
                    after_publish();
                return; // finish this one before the next
            }
            if (preparation) {
                if (before_prepare)
                    before_prepare();
                (void)preparation->prepare();
                if (stop.stop_requested())
                    return;
                auto result = std::make_unique<PreparedResult>(
                        command, std::move(preparation));
                // we already delivered the last one, audio can only free up
                // room
                if (!prepared_results.emplace(std::move(result)))
                    throw std::logic_error("IR result mailbox still occupied");
                request_processing();
                if (after_publish)
                    after_publish();
                return; // deliver it before the next one
            }
            // no locks left, so the old convolver and friends get freed here
        }
    }
};
} // namespace ekphore

namespace {

void inline_svg_class_styles(std::string &source) {
    // NanoSVG doesn't do CSS classes and NI puts the fill in .cls-N rules, so
    // inline them or we lose the colors
    std::unordered_map<std::string, std::string> styles;
    std::size_t cursor{};
    while ((cursor = source.find('.', cursor)) != std::string::npos) {
        const auto name_begin = cursor + 1;
        auto name_end = name_begin;
        while (name_end < source.size() &&
                (std::isalnum(static_cast<unsigned char>(source[name_end])) ||
                        source[name_end] == '-' || source[name_end] == '_'))
            ++name_end;
        auto brace = source.find('{', name_end);
        if (name_end == name_begin || brace == std::string::npos) {
            ++cursor;
            continue;
        }
        const auto between =
                std::string_view{ source }.substr(name_end, brace - name_end);
        if (between.find_first_not_of(" \t\r\n") != std::string_view::npos) {
            cursor = name_end;
            continue;
        }
        const auto close = source.find('}', brace + 1);
        if (close == std::string::npos)
            break;
        styles.emplace(source.substr(name_begin, name_end - name_begin),
                source.substr(brace + 1, close - brace - 1));
        cursor = close + 1;
    }
    for (const auto &[name, style] : styles) {
        const auto attribute = "class=\"" + name + "\"";
        const auto replacement = "style=\"" + style + "\"";
        std::size_t position{};
        while ((position = source.find(attribute, position)) !=
                std::string::npos) {
            source.replace(position, attribute.size(), replacement);
            position += replacement.size();
        }
    }
}

constexpr std::size_t loader_script_slot = 0;
constexpr std::int64_t loader_path_id = -1001;
constexpr std::int64_t loader_button_id = -1002;
constexpr std::string_view loader_path_name = "@ekphore_instrument_path";
constexpr std::string_view loader_button_name = "$ekphore_load_instrument";

std::size_t previous_utf8_boundary(
        const std::string_view text, std::size_t position) {
    position = std::min(position, text.size());
    if (position == 0)
        return 0;
    --position;
    while (position > 0 &&
            (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U)
        --position;
    return position;
}

std::size_t next_utf8_boundary(
        const std::string_view text, std::size_t position) {
    position = std::min(position, text.size());
    if (position == text.size())
        return position;
    ++position;
    while (position < text.size() &&
            (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U)
        ++position;
    return position;
}

std::string kontakt_ascii(const std::string_view text) {
    // one byte outside 0x01..0x7f and the whole thing is empty, no
    // transliteration
    if (std::ranges::any_of(text, [](const unsigned char character) {
            return character == 0 || character > 0x7fU;
        }))
        return {};
    return std::string(text);
}

std::shared_ptr<const ekphore::UiImage> decode_png(
        const std::vector<std::byte> &bytes) {
    if (bytes.empty())
        return {};
    png_image source{};
    source.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&source, bytes.data(), bytes.size()))
        return {};
    source.format = PNG_FORMAT_RGBA;
    ekphore::UiImage image;
    image.width = source.width;
    image.height = source.height;
    image.rgba.resize(PNG_IMAGE_SIZE(source));
    if (!png_image_finish_read(
                &source, nullptr, image.rgba.data(), 0, nullptr)) {
        png_image_free(&source);
        return {};
    }
    png_image_free(&source);
    return image.valid()
                   ? std::make_shared<const ekphore::UiImage>(std::move(image))
                   : nullptr;
}

std::shared_ptr<const ekphore::UiImage> decode_webp(
        const std::vector<std::byte> &bytes) {
    if (bytes.empty())
        return {};
    int width{};
    int height{};
    if (!WebPGetInfo(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                bytes.size(), &width, &height) ||
            width <= 0 || height <= 0)
        return {};
    ekphore::UiImage image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    if (!WebPDecodeRGBAInto(
                reinterpret_cast<const std::uint8_t *>(bytes.data()),
                bytes.size(), image.rgba.data(), image.rgba.size(), width * 4))
        return {};
    return std::make_shared<const ekphore::UiImage>(std::move(image));
}

struct TiffMemoryReader {
    const std::byte *data{};
    std::size_t size{};
    std::size_t position{};
};

tsize_t tiff_read(thandle_t handle, void *destination, const tsize_t count) {
    auto &source = *static_cast<TiffMemoryReader *>(handle);
    const auto available = source.size - std::min(source.position, source.size);
    const auto copied = std::min<std::size_t>(
            available, static_cast<std::size_t>(std::max<tsize_t>(count, 0)));
    std::memcpy(destination, source.data + source.position, copied);
    source.position += copied;
    return static_cast<tsize_t>(copied);
}

tsize_t tiff_write(thandle_t, void *, tsize_t) { return 0; }

int tiff_close(thandle_t) { return 0; }

toff_t tiff_seek(thandle_t handle, const toff_t offset, const int origin) {
    auto &source = *static_cast<TiffMemoryReader *>(handle);
    std::size_t base{};
    if (origin == SEEK_CUR)
        base = source.position;
    else if (origin == SEEK_END)
        base = source.size;
    const auto next = base + static_cast<std::size_t>(offset);
    source.position = std::min(next, source.size);
    return static_cast<toff_t>(source.position);
}

toff_t tiff_size(thandle_t handle) {
    return static_cast<toff_t>(static_cast<TiffMemoryReader *>(handle)->size);
}

int tiff_map(thandle_t handle, void **base, toff_t *size) {
    const auto &source = *static_cast<TiffMemoryReader *>(handle);
    *base = const_cast<std::byte *>(source.data);
    *size = static_cast<toff_t>(source.size);
    return 1;
}

void tiff_unmap(thandle_t, void *, toff_t) {}

std::shared_ptr<const ekphore::UiImage> decode_tiff(
        const std::vector<std::byte> &bytes) {
    if (bytes.size() < 8)
        return {};
    const bool little = std::memcmp(bytes.data(), "II\x2a\0", 4) == 0;
    const bool big = std::memcmp(bytes.data(), "MM\0\x2a", 4) == 0;
    if (!little && !big)
        return {};
    TiffMemoryReader source{ bytes.data(), bytes.size(), 0 };
    auto *tiff = TIFFClientOpen("memory", "rm", &source, tiff_read, tiff_write,
            tiff_seek, tiff_close, tiff_size, tiff_map, tiff_unmap);
    if (!tiff)
        return {};
    std::uint32_t width{};
    std::uint32_t height{};
    const bool dimensions = TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) &&
                            TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    std::vector<std::uint32_t> raster;
    if (dimensions && width > 0 && height > 0)
        raster.resize(static_cast<std::size_t>(width) * height);
    const bool decoded =
            !raster.empty() && TIFFReadRGBAImageOriented(tiff, width, height,
                                       raster.data(), ORIENTATION_TOPLEFT, 0);
    TIFFClose(tiff);
    if (!decoded)
        return {};
    ekphore::UiImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(raster.size() * 4);
    for (std::size_t index = 0; index < raster.size(); ++index) {
        image.rgba[index * 4] = TIFFGetR(raster[index]);
        image.rgba[index * 4 + 1] = TIFFGetG(raster[index]);
        image.rgba[index * 4 + 2] = TIFFGetB(raster[index]);
        image.rgba[index * 4 + 3] = TIFFGetA(raster[index]);
    }
    return std::make_shared<const ekphore::UiImage>(std::move(image));
}

std::shared_ptr<const ekphore::UiImage> decode_tga(
        const std::vector<std::byte> &bytes) {
    if (bytes.size() < 18)
        return {};
    const auto byte = [&](const std::size_t offset) {
        return std::to_integer<std::uint8_t>(bytes[offset]);
    };
    const auto word = [&](const std::size_t offset) {
        return static_cast<std::uint16_t>(
                byte(offset) |
                (static_cast<std::uint16_t>(byte(offset + 1)) << 8U));
    };
    const auto image_type = byte(2);
    const auto width = word(12);
    const auto height = word(14);
    const auto depth = byte(16);
    if (byte(1) != 0 || (image_type != 2 && image_type != 10) || width == 0 ||
            height == 0 || (depth != 24 && depth != 32))
        return {};
    std::size_t cursor = 18U + byte(0);
    const auto source_stride = static_cast<std::size_t>(depth / 8U);
    const auto pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<std::array<std::uint8_t, 4>> pixels;
    pixels.reserve(pixel_count);
    const auto read_pixel =
            [&]() -> std::optional<std::array<std::uint8_t, 4>> {
        if (cursor + source_stride > bytes.size())
            return std::nullopt;
        const std::array result{ byte(cursor + 2), byte(cursor + 1),
            byte(cursor),
            static_cast<std::uint8_t>(depth == 32 ? byte(cursor + 3) : 255) };
        cursor += source_stride;
        return result;
    };
    if (image_type == 2) {
        while (pixels.size() < pixel_count) {
            const auto pixel = read_pixel();
            if (!pixel)
                return {};
            pixels.push_back(*pixel);
        }
    } else {
        while (pixels.size() < pixel_count) {
            if (cursor >= bytes.size())
                return {};
            const auto packet = byte(cursor++);
            const auto count = static_cast<std::size_t>((packet & 0x7fU) + 1U);
            if (count > pixel_count - pixels.size())
                return {};
            if ((packet & 0x80U) != 0) {
                const auto pixel = read_pixel();
                if (!pixel)
                    return {};
                pixels.insert(pixels.end(), count, *pixel);
            } else {
                for (std::size_t index = 0; index < count; ++index) {
                    const auto pixel = read_pixel();
                    if (!pixel)
                        return {};
                    pixels.push_back(*pixel);
                }
            }
        }
    }
    ekphore::UiImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(pixel_count * 4U);
    const bool top_origin = (byte(17) & 0x20U) != 0;
    const bool right_origin = (byte(17) & 0x10U) != 0;
    for (std::size_t source_y = 0; source_y < height; ++source_y)
        for (std::size_t source_x = 0; source_x < width; ++source_x) {
            const auto destination_x =
                    right_origin
                            ? static_cast<std::size_t>(width) - 1U - source_x
                            : source_x;
            const auto destination_y =
                    top_origin
                            ? source_y
                            : static_cast<std::size_t>(height) - 1U - source_y;
            const auto &pixel = pixels[source_y * width + source_x];
            const auto destination =
                    (destination_y * width + destination_x) * 4U;
            std::copy(pixel.begin(), pixel.end(),
                    image.rgba.begin() + destination);
        }
    return std::make_shared<const ekphore::UiImage>(std::move(image));
}

std::shared_ptr<const ekphore::UiImage> decode_svg(
        const std::vector<std::byte> &bytes, const float requested_scale) {
    if (bytes.empty())
        return {};
    std::string text(
            reinterpret_cast<const char *>(bytes.data()), bytes.size());
    inline_svg_class_styles(text);
    std::vector<char> source(text.begin(), text.end());
    source.push_back('\0');
    auto *svg = nsvgParse(source.data(), "px", 96.0F);
    if (!svg || svg->width <= 0.0F || svg->height <= 0.0F) {
        if (svg)
            nsvgDelete(svg);
        return {};
    }
    const auto scale = std::clamp(requested_scale, 1.0F, 4.0F);
    const auto width =
            static_cast<std::uint32_t>(std::ceil(svg->width * scale));
    const auto height =
            static_cast<std::uint32_t>(std::ceil(svg->height * scale));
    ekphore::UiImage image;
    image.width = width;
    image.height = height;
    image.pixel_density = scale;
    image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    auto *rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) {
        nsvgDelete(svg);
        return {};
    }
    nsvgRasterize(rasterizer, svg, 0.0F, 0.0F, scale, image.rgba.data(),
            static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(width * 4));
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(svg);
    return image.valid()
                   ? std::make_shared<const ekphore::UiImage>(std::move(image))
                   : nullptr;
}

std::shared_ptr<const ekphore::UiImage> decode_ui_image(
        const std::vector<std::byte> &bytes, const float scale = 1.0F) {
    if (bytes.size() >= 8 &&
            std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        return decode_png(bytes);
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
            std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
        return decode_webp(bytes);
    if (const auto tiff = decode_tiff(bytes))
        return tiff;
    if (const auto tga = decode_tga(bytes))
        return tga;
    const std::string_view text(
            reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (text.find("<svg") != std::string_view::npos)
        return decode_svg(bytes, scale);
    return {};
}

void apply_image_metadata(
        ekphore::UiImage &image, const std::vector<std::byte> &bytes) {
    const std::string_view source(
            reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!ekphore::detail::classic_picture_metadata_complete(source))
        return;
    const auto value_after = [&](const std::string_view key) {
        const auto start = source.find(key);
        if (start == std::string_view::npos)
            return std::string_view{};
        const auto value = start + key.size();
        const auto end = source.find_first_of("\r\n", value);
        return source.substr(value, end == std::string_view::npos
                                            ? source.size() - value
                                            : end - value);
    };
    const auto animations = value_after("Number of Animations:");
    std::uint32_t count{};
    const auto first = animations.find_first_not_of(' ');
    if (first != std::string_view::npos) {
        const auto number = animations.substr(first);
        if (std::from_chars(number.data(), number.data() + number.size(), count)
                                .ec == std::errc{} &&
                count > 0)
            image.animation_count = count;
    }
    const auto horizontal = value_after("Horizontal Animation:");
    image.horizontal_animation =
            horizontal.find("yes") != std::string_view::npos;
    image.vertical_resizable = value_after("Vertical Resizable:").find("yes") !=
                               std::string_view::npos;
    image.horizontal_resizable =
            value_after("Horizontal Resizable:").find("yes") !=
            std::string_view::npos;
    const auto read_size = [&](const std::string_view key) {
        const auto value = value_after(key);
        std::uint32_t result{};
        const auto first = value.find_first_not_of(' ');
        if (first == std::string_view::npos)
            return result;
        const auto number = value.substr(first);
        (void)std::from_chars(
                number.data(), number.data() + number.size(), result);
        return result;
    };
    const auto scaled = [&](const std::string_view key) {
        return static_cast<std::uint32_t>(
                std::lround(read_size(key) * image.pixel_density));
    };
    image.fixed_top = scaled("Fixed Top:");
    image.fixed_bottom = scaled("Fixed Bottom:");
    image.fixed_left = scaled("Fixed Left:");
    image.fixed_right = scaled("Fixed Right:");
}

class PlayerAssetProvider final : public ekphore::UiAssetProvider {
  public:
    PlayerAssetProvider(ekphore::Player::UiAssetReader reader,
            std::shared_ptr<ekphore::PluginUiAssetCache> cache)
        : asset_reader(std::move(reader)), asset_cache(std::move(cache)) {}

    std::shared_ptr<const ekphore::UiImage> image(
            const std::string_view name) const override {
        return image(name, 1.0F);
    }

    std::shared_ptr<const ekphore::UiImage> image(const std::string_view name,
            const float pixel_density) const override {
        const auto density = std::clamp(pixel_density, 1.0F, 4.0F);
        const std::string asset_name(name);
        const std::string key = asset_name + "#" +
                                std::to_string(static_cast<int>(
                                        std::lround(density * 1024.0F)));
        {
            std::scoped_lock lock(asset_cache->mutex);
            if (const auto found = asset_cache->images.find(key);
                    found != asset_cache->images.end()) {
                if (std::getenv("EKPHORE_UI_DIAGNOSTICS_ALL") && found->second)
                    std::cerr << "ui-image-cache=" << asset_name
                              << " size=" << found->second->width << 'x'
                              << found->second->height
                              << " frames=" << found->second->animation_count
                              << " horizontal="
                              << found->second->horizontal_animation
                              << " resizable="
                              << found->second->horizontal_resizable << ','
                              << found->second->vertical_resizable
                              << " fixed=" << found->second->fixed_left << ','
                              << found->second->fixed_right << ','
                              << found->second->fixed_top << ','
                              << found->second->fixed_bottom
                              << " density=" << found->second->pixel_density
                              << '\n';
                return found->second;
            }
        }
        auto decoded = asset_reader(name);
        auto result = decoded ? decode_ui_image(*decoded, density) : nullptr;
        if (result) {
            auto editable = std::make_shared<ekphore::UiImage>(*result);
            editable->opaque = true;
            for (std::size_t alpha = 3;
                    editable->opaque && alpha < editable->rgba.size();
                    alpha += 4)
                editable->opaque = editable->rgba[alpha] == 255;
            const std::string metadata = asset_name + ".txt";
            if (const auto bytes = asset_reader(metadata)) {
                apply_image_metadata(*editable, *bytes);
            }
            if (std::getenv("EKPHORE_UI_DIAGNOSTICS"))
                std::cerr << "ui-image=" << asset_name
                          << " size=" << editable->width << 'x'
                          << editable->height
                          << " frames=" << editable->animation_count
                          << " horizontal=" << editable->horizontal_animation
                          << " resizable=" << editable->horizontal_resizable
                          << ',' << editable->vertical_resizable << '\n';
            result = std::move(editable);
        }
        std::scoped_lock lock(asset_cache->mutex);
        return asset_cache->images.emplace(key, result).first->second;
    }

    std::shared_ptr<const std::vector<std::byte>> data(
            const std::string_view name) const override {
        const std::string key(name);
        {
            std::scoped_lock lock(asset_cache->mutex);
            if (const auto found = asset_cache->data.find(key);
                    found != asset_cache->data.end())
                return found->second;
        }
        auto bytes = asset_reader(name);
        auto result = bytes ? std::make_shared<const std::vector<std::byte>>(
                                      std::move(*bytes))
                            : std::shared_ptr<const std::vector<std::byte>>{};
        std::scoped_lock lock(asset_cache->mutex);
        return asset_cache->data.emplace(key, result).first->second;
    }

  private:
    ekphore::Player::UiAssetReader asset_reader;
    std::shared_ptr<ekphore::PluginUiAssetCache> asset_cache;
};

} // namespace

namespace ekphore {

Plugin::Plugin() : Plugin(std::shared_ptr<nks::Client>{}) {}

Plugin::Plugin(std::shared_ptr<nks::Client> keyboard_client)
    : parameter_publication(std::make_unique<PluginParameterPublication>()),
      event_scratch(std::make_unique<PluginEventScratch>()),
      ui_commands(std::make_unique<PluginUiCommands>()),
      ui_assets(std::make_shared<PluginUiAssetCache>()) {
    command_service = std::make_unique<PluginCommandService>(
            *this, std::move(keyboard_client));
    ui_commands->notify = [service = command_service.get()] {
        service->requested.store(true);
        service->wake.notify_all();
    };
}
Plugin::~Plugin() = default;

void Plugin::set_processing_request(std::function<void()> request) {
    if (!command_service)
        return;
    std::scoped_lock lock(command_service->processing_request_mutex);
    command_service->processing_request = std::move(request);
}

bool Plugin::submit_ui_control(const std::size_t slot,
        const std::string_view name, const libksp::Value &value) {
    const auto target =
            ui_readback ? ui_readback->target(slot, name) : std::nullopt;
    if (!target)
        return false;
    if (!ui_commands)
        ui_commands = std::make_unique<PluginUiCommands>();
    auto &queue = *ui_commands;
    // a full ring keeps the rest in order on our side, never run callbacks
    // just because it's full
    return queue.submit(PluginUiCommands::Command{ *target, value, false,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, {}, {} });
}

bool Plugin::submit_native_ui_writes(Player::NativeUiWriteBatch batch) {
    // Lua already ran, only the payload goes through, never registry IDs or
    // closures
    if (!ui_commands)
        ui_commands = std::make_unique<PluginUiCommands>();
    auto &queue = *ui_commands;
    PluginUiCommands::Command command{};
    command.native.emplace(std::move(batch));
    return queue.submit(std::move(command));
}

bool Plugin::submit_ui_xy(const std::size_t slot, const std::string_view name,
        const std::size_t first, const std::array<double, 2> &coordinates,
        const int mouse_event_type) {
    const auto target =
            ui_readback ? ui_readback->target(slot, name) : std::nullopt;
    if (!target)
        return false;
    if (!ui_commands)
        ui_commands = std::make_unique<PluginUiCommands>();
    PluginUiCommands::Command command{};
    command.target = *target;
    command.xy.emplace(first, coordinates);
    command.xy_mouse_event_type = mouse_event_type;
    return ui_commands->submit(std::move(command));
}

bool Plugin::submit_native_ui_gesture(
        const std::uint64_t action, const NativeUiGestureEvent &event) {
    std::unique_lock lock(ui_mutex);
    return submit_native_ui_gesture(action, event, lock);
}

bool Plugin::submit_native_ui_control(
        const std::string_view control, const double normalized_value) {
    if (!ui_readback)
        return false;
    auto batch =
            native_ui_session->prepare(*ui_readback, control, normalized_value);
    if (!batch)
        return false;
    const bool accepted = submit_native_ui_writes(std::move(*batch));
    if (accepted && ui_commands)
        ui_commands->native_readback_dirty = true;
    return accepted;
}

void Plugin::drain_ui_controls(const bool producer_barrier) const {
    consume_host_tempo();
    if (!ui_commands)
        return;
    auto &commands = ui_commands->commands;
    if (!producer_barrier) {
        const auto count = commands.available();
        drain_ui_control_batch(count);
        if (count) {
            publish_parameters();
            ui_commands->native_readback_dirty = true;
        }
        return;
    }
    // every accepted edit, even overflow. take the count now or callbacks keep
    // us in here
    auto remaining = ui_commands->accepted_count();
    const bool changed = remaining != 0;
    while (remaining) {
        ui_commands->pump();
        const auto count = std::min(remaining, commands.available());
        drain_ui_control_batch(count);
        remaining -= count;
    }
    if (changed) {
        publish_parameters();
        ui_commands->native_readback_dirty = true;
    }
}

void Plugin::drain_ui_control_batch(const std::size_t count) const {
    // we hold engine_owner, if a callback queues more it'll wait for the
    // next drain. don't reset the slots, audio is still using them
    if (!ui_commands)
        return;
    auto &queue = *ui_commands;
    if (count)
        queue.native_readback_dirty = true;
    // apply the writes first otherwise callbacks see stale values
    std::size_t consumed{};
    // mark it consumed before calling it so a throw doesn't run it twice,
    // we give the slots back once we're done with them
    struct Release {
        decltype(queue.commands) &commands;
        std::size_t &consumed;
        ~Release() { commands.release(consumed); }
    } release{ queue.commands, consumed };
    const auto retire = [&] {
        queue.completed_sequence = queue.commands.at(consumed).sequence;
        --queue.pending_count;
        ++consumed;
    };
    while (consumed < count) {
        auto &first = queue.commands.at(consumed);
        if (first.waveform) {
            retire();
            (void)instrument_player.apply_waveform_edit(first.target,
                    first.waveform->first, first.waveform->second);
            continue;
        }
        if (first.reset_generation) {
            retire();
            if (*first.reset_generation ==
                    instrument_player.engine_generation())
                apply_reset();
            continue;
        }
        if (first.performance_view) {
            retire();
            (void)instrument_player.apply_performance_view_selection(
                    *first.performance_view);
            continue;
        }
        if (first.native) {
            // same as above, retire BEFORE dispatch
            retire();
            (void)instrument_player.apply_native_ui_writes(*first.native);
            continue;
        }
        if (first.xy) {
            retire();
            (void)instrument_player.apply_script_control_xy(first.target,
                    first.xy->first, first.xy->second,
                    first.xy_mouse_event_type);
            continue;
        }
        // stop at a native batch or we'd reorder submissions
        std::size_t run{};
        while (consumed + run < count) {
            auto &command = queue.commands.at(consumed + run);
            if (command.native || command.xy || command.performance_view ||
                    command.reset_generation || command.waveform)
                break;
            command.notify =
                    command.mouse ? instrument_player.assign_mouse_area_event(
                                            command.target, *command.mouse)
                    : command.file_selection
                            ? instrument_player.assign_file_selector_event(
                                      command.target, *command.file_selection)
                            : instrument_player.assign_script_control_value(
                                      command.target, command.value);
            ++run;
        }
        for (std::size_t i = 0; i < run; ++i) {
            auto &command = queue.commands.at(consumed);
            retire();
            if (command.notify)
                (void)instrument_player.dispatch_script_control_callback(
                        command.target);
        }
    }
}

bool Plugin::submit_native_ui_gesture(const std::uint64_t action,
        const NativeUiGestureEvent &event,
        std::unique_lock<std::recursive_mutex> &lock) {
    if (!read_input_document(lock))
        return false;
    auto job = native_ui_session->gesture(*ui_readback, action, event);
    if (!job)
        return false;
    lock.unlock();
    try {
        Player::evaluate_native_ui_gesture(*job);
    } catch (...) {
        job.reset();
        lock.lock();
        throw;
    }
    lock.lock();
    auto batch = native_ui_session->stage(*job);
    const bool accepted = batch && submit_native_ui_writes(std::move(*batch));
    if (accepted && ui_commands)
        ui_commands->native_readback_dirty = true;
    lock.unlock();
    job.reset(); // could be the last ref to the old runtime
    lock.lock();
    if (accepted && !native_hover_dispatching)
        refresh_editor_readback(lock);
    return accepted;
}

Plugin::Plugin(Plugin &&other) noexcept {
    if (other.load_job && other.load_job->worker.joinable())
        other.load_job->worker.join();
    other.load_job.reset();
    std::unique_lock<std::mutex> binding;
    if (other.command_service)
        binding = std::unique_lock(other.command_service->owner_mutex);
    std::scoped_lock lock(other.engine_owner);
    std::scoped_lock ui(other.ui_mutex);
    instrument_player = std::move(other.instrument_player);
    parameter_publication = std::move(other.parameter_publication);
    nks_generation = other.nks_generation;
    input_publication = std::move(other.input_publication);
    native_ui_session = std::move(other.native_ui_session);
    ui_readback = std::move(other.ui_readback);
    editor_resources = std::move(other.editor_resources);
    editor_resources_dirty = other.editor_resources_dirty.load();
    pending_host_tempo.store(other.pending_host_tempo.exchange(0.0));
    render_buffer = std::move(other.render_buffer);
    event_scratch = std::move(other.event_scratch);
    ui_commands = std::move(other.ui_commands);
    native_rebuild_pending = std::exchange(other.native_rebuild_pending, false);
    processing_rate = other.processing_rate;
    block_limit = other.block_limit;
    is_active = other.is_active;
    audio_service_deadline = other.audio_service_deadline;
    captured_control = std::move(other.captured_control);
    last_drag_position = std::move(other.last_drag_position);
    waveform_drag = std::move(other.waveform_drag);
    captured_drag_normalized = std::move(other.captured_drag_normalized);
    opened_menu = std::move(other.opened_menu);
    menu_hover_index = other.menu_hover_index;
    menu_pressed_index = other.menu_pressed_index;
    menu_scroll_offset = other.menu_scroll_offset;
    menu_last_scroll = other.menu_last_scroll;
    hovered_control = std::move(other.hovered_control);
    focused_text_edit = std::move(other.focused_text_edit);
    text_edit_buffer = std::move(other.text_edit_buffer);
    text_edit_original = std::move(other.text_edit_original);
    text_edit_caret = other.text_edit_caret;
    text_edit_selection_anchor = std::move(other.text_edit_selection_anchor);
    ui_assets = other.ui_assets;
    instrument_path = std::move(other.instrument_path);
    has_instrument = other.has_instrument;
    pending_instrument_path = std::move(other.pending_instrument_path);
    instrument_load_error = std::move(other.instrument_load_error);
    before_command_prepare = std::move(other.before_command_prepare);
    after_command_result_publish =
            std::move(other.after_command_result_publish);
    before_ui_queue_maintenance = std::move(other.before_ui_queue_maintenance);
    before_instrument_prepare = std::move(other.before_instrument_prepare);
    command_service = std::move(other.command_service);
    if (command_service)
        command_service->owner = this;
    other.has_instrument = false;
    other.is_active = false;
}

Plugin &Plugin::operator=(Plugin &&other) noexcept {
    if (this == &other)
        return *this;
    load_job.reset();
    other.load_job.reset();
    {
        std::scoped_lock ui(ui_mutex);
        if (ui_commands)
            ui_commands->notify = {};
    }
    command_service.reset(); // joins, so NOT with the engine lock
    std::unique_ptr<PluginNativeUiSession> retired_ui;
    std::unique_lock<std::mutex> binding;
    if (other.command_service)
        binding = std::unique_lock(other.command_service->owner_mutex);
    std::scoped_lock lock(engine_owner, other.engine_owner);
    std::scoped_lock ui(ui_mutex, other.ui_mutex);
    instrument_player = std::move(other.instrument_player);
    parameter_publication = std::move(other.parameter_publication);
    nks_generation = other.nks_generation;
    input_publication = std::move(other.input_publication);
    retired_ui = std::move(native_ui_session);
    native_ui_session = std::move(other.native_ui_session);
    ui_readback = std::move(other.ui_readback);
    editor_resources = std::move(other.editor_resources);
    editor_resources_dirty = other.editor_resources_dirty.load();
    pending_host_tempo.store(other.pending_host_tempo.exchange(0.0));
    render_buffer = std::move(other.render_buffer);
    event_scratch = std::move(other.event_scratch);
    ui_commands = std::move(other.ui_commands);
    native_rebuild_pending = std::exchange(other.native_rebuild_pending, false);
    processing_rate = other.processing_rate;
    block_limit = other.block_limit;
    is_active = other.is_active;
    audio_service_deadline = other.audio_service_deadline;
    captured_control = std::move(other.captured_control);
    last_drag_position = std::move(other.last_drag_position);
    waveform_drag = std::move(other.waveform_drag);
    captured_drag_normalized = std::move(other.captured_drag_normalized);
    opened_menu = std::move(other.opened_menu);
    menu_hover_index = other.menu_hover_index;
    menu_pressed_index = other.menu_pressed_index;
    menu_scroll_offset = other.menu_scroll_offset;
    menu_last_scroll = other.menu_last_scroll;
    hovered_control = std::move(other.hovered_control);
    focused_text_edit = std::move(other.focused_text_edit);
    text_edit_buffer = std::move(other.text_edit_buffer);
    text_edit_original = std::move(other.text_edit_original);
    text_edit_caret = other.text_edit_caret;
    text_edit_selection_anchor = std::move(other.text_edit_selection_anchor);
    ui_assets = other.ui_assets;
    ++editor_render_revision;
    native_ui_hits.clear();
    captured_native_ui_hit.reset();
    hovered_native_ui_hits.clear();
    instrument_path = std::move(other.instrument_path);
    has_instrument = other.has_instrument;
    pending_instrument_path = std::move(other.pending_instrument_path);
    instrument_load_error = std::move(other.instrument_load_error);
    before_command_prepare = std::move(other.before_command_prepare);
    after_command_result_publish =
            std::move(other.after_command_result_publish);
    before_ui_queue_maintenance = std::move(other.before_ui_queue_maintenance);
    before_instrument_prepare = std::move(other.before_instrument_prepare);
    command_service = std::move(other.command_service);
    if (command_service)
        command_service->owner = this;
    other.has_instrument = false;
    other.is_active = false;
    return *this;
}

bool Plugin::command_service_failed() const {
    std::scoped_lock lock(engine_owner);
    return command_service &&
           command_service->failed.load(std::memory_order_acquire);
}

const PluginDescriptor &Plugin::descriptor() {
    static const PluginDescriptor value;
    return value;
}

LoadReport Plugin::load(const std::filesystem::path &instrument,
        const std::optional<std::size_t> performance_view_script,
        const InstrumentLoadMode mode) {
    return load_prepared_path(instrument, performance_view_script, {}, mode);
}

LoadReport Plugin::load_prepared_path(const std::filesystem::path &instrument,
        const std::optional<std::size_t> performance_view_script,
        const std::function<void(Player &)> &restore,
        const InstrumentLoadMode mode) {
    struct Loading {
        std::atomic<unsigned> &count;
        Loading(std::atomic<unsigned> &c) : count(c) { ++count; }
        ~Loading() { --count; }
    } loading(nks_loading);
    std::scoped_lock replacement(replacement_mutex);
    {
        std::scoped_lock ui(ui_mutex);
        nks_loading_name = instrument.stem().string();
    }
    std::uint32_t prepared_capacity{};
    std::function<void()> before_prepare;
    {
        std::scoped_lock lock(engine_owner);
        prepared_capacity = block_limit;
        before_prepare = before_instrument_prepare;
    }
    // get rid of the old engine first, here and NOT on audio
    {
        std::optional<Player> retired;
        std::scoped_lock lock(engine_owner, ui_mutex);
        drain_ui_controls();
        consume_host_tempo();
        retired.emplace(std::move(instrument_player));
        instrument_player = Player{};
        has_instrument = false;
        instrument_path.reset();
        prepare_parameter_publication();
    }
    if (before_prepare)
        before_prepare();
    Player prepared;
    const auto report =
            prepared.load(instrument, performance_view_script, mode);
    if (!report.ok()) {
        std::scoped_lock ui(ui_mutex);
        instrument_load_error = report.diagnostics.empty()
                                        ? "Could not load that instrument."
                                        : report.diagnostics.front();
        return report; // the slot stays empty
    }
    if (report.ok() && restore)
        restore(prepared);
    prepared.prepare_render(prepared_capacity);
    auto assets = std::make_shared<PluginUiAssetCache>();
    assets->reader = prepared.ui_asset_reader();
    std::optional<Player> retired;
    std::unique_ptr<PluginNativeUiSession> retired_ui;
    std::unique_lock lock(engine_owner);
    while (is_active && block_limit > prepared_capacity) {
        prepared_capacity = block_limit;
        lock.unlock();
        prepared.prepare_render(prepared_capacity);
        lock.lock();
    }
    std::scoped_lock ui(ui_mutex);
    if (restore)
        drain_ui_controls();
    consume_host_tempo(); // this one goes to the old Player
    if (!command_service)
        command_service = std::make_unique<PluginCommandService>(*this);
    captured_control.reset();
    captured_xy.reset();
    hovered_xy = -1;
    last_drag_position.reset();
    captured_drag_normalized.reset();
    opened_menu.reset();
    hovered_control.reset();
    focused_text_edit.reset();
    text_edit_buffer.clear();
    text_edit_original.clear();
    ui_assets = std::move(assets);
    ++editor_render_revision;
    native_ui_hits.clear();
    captured_native_ui_hit.reset();
    hovered_native_ui_hits.clear();
    retired.emplace(std::move(instrument_player));
    instrument_player = std::move(prepared);
    native_rebuild_pending = false;
    if (report.ok()) {
        instrument_path = instrument;
        has_instrument = true;
        pending_instrument_path = instrument.string();
        instrument_load_error.clear();
    } else {
        instrument_load_error = report.diagnostics.empty()
                                        ? "Could not load that instrument."
                                        : report.diagnostics.front();
    }
    retired_ui = std::move(native_ui_session);
    prepare_parameter_publication();
    return report;
}

LoadReport Plugin::load(libnki::InstrumentInfo instrument,
        const std::optional<std::size_t> performance_view_script) {
    struct Loading {
        std::atomic<unsigned> &count;
        Loading(std::atomic<unsigned> &c) : count(c) { ++count; }
        ~Loading() { --count; }
    } loading(nks_loading);
    std::scoped_lock replacement(replacement_mutex);
    std::uint32_t prepared_capacity{};
    std::function<void()> before_prepare;
    {
        std::scoped_lock lock(engine_owner);
        prepared_capacity = block_limit;
        before_prepare = before_instrument_prepare;
    }
    if (before_prepare)
        before_prepare();
    Player prepared;
    const auto report =
            prepared.load(std::move(instrument), performance_view_script);
    // a broken view doesn't replace the engine
    const bool replace_player = prepared.has_instrument_model();
    prepared.prepare_render(prepared_capacity);
    auto assets = std::make_shared<PluginUiAssetCache>();
    if (replace_player)
        assets->reader = prepared.ui_asset_reader();
    std::optional<Player> retired;
    std::unique_ptr<PluginNativeUiSession> retired_ui;
    std::unique_lock lock(engine_owner);
    while (replace_player && is_active && block_limit > prepared_capacity) {
        prepared_capacity = block_limit;
        lock.unlock();
        prepared.prepare_render(prepared_capacity);
        lock.lock();
    }
    std::scoped_lock ui(ui_mutex);
    consume_host_tempo();
    if (!command_service)
        command_service = std::make_unique<PluginCommandService>(*this);
    captured_control.reset();
    captured_xy.reset();
    hovered_xy = -1;
    last_drag_position.reset();
    captured_drag_normalized.reset();
    opened_menu.reset();
    hovered_control.reset();
    focused_text_edit.reset();
    text_edit_buffer.clear();
    text_edit_original.clear();
    ui_assets = std::move(assets);
    ++editor_render_revision;
    native_ui_hits.clear();
    captured_native_ui_hit.reset();
    hovered_native_ui_hits.clear();
    instrument_path.reset();
    if (replace_player) {
        retired.emplace(std::move(instrument_player));
        instrument_player = std::move(prepared);
    } else {
        ui_assets->reader = instrument_player.ui_asset_reader();
    }
    native_rebuild_pending = false;
    has_instrument = report.ok();
    instrument_load_error = report.ok() || report.diagnostics.empty()
                                    ? std::string{}
                                    : report.diagnostics.front();
    retired_ui = std::move(native_ui_session);
    prepare_parameter_publication();
    return report;
}

void Plugin::activate(
        const double sample_rate, const std::uint32_t maximum_block_size) {
    std::scoped_lock lock(engine_owner);
    if (!event_scratch)
        event_scratch = std::make_unique<PluginEventScratch>();
    processing_rate = std::clamp(sample_rate, 8000.0, 768000.0);
    block_limit = std::max(maximum_block_size, 1U);
    instrument_player.prepare_render(block_limit);
    render_buffer.left.reserve(block_limit);
    render_buffer.right.reserve(block_limit);
    is_active = true;
    audio_service_deadline = {};
}

bool Plugin::audio_processing_recent() const {
    // just a hint for scheduling, engine_owner is still what protects us
    return is_active &&
           std::chrono::steady_clock::now() < audio_service_deadline;
}

void Plugin::deactivate() {
    std::scoped_lock lock(engine_owner);
    std::scoped_lock ui(ui_mutex);
    drain_ui_controls();
    is_active = false;
    captured_control.reset();
    captured_xy.reset();
    last_drag_position.reset();
    captured_drag_normalized.reset();
    focused_text_edit.reset();
    text_edit_buffer.clear();
    text_edit_original.clear();
}

void Plugin::reset() {
    std::scoped_lock lock(engine_owner);
    std::scoped_lock ui(ui_mutex);
    if (is_active) {
        if (!ui_commands)
            ui_commands = std::make_unique<PluginUiCommands>();
        PluginUiCommands::Command command{};
        command.reset_generation = instrument_player.engine_generation();
        (void)ui_commands->submit(std::move(command));
        return;
    }
    drain_ui_controls();
    apply_reset();
    publish_parameters();
}

void Plugin::apply_reset() const {
    // a transport reset only releases notes, everything else stays
    for (std::uint16_t note = 0; note < 128; ++note)
        (void)instrument_player.note_off(static_cast<std::uint8_t>(note));
}

void Plugin::set_tempo(const double beats_per_minute) {
    static_assert(std::atomic<double>::is_always_lock_free);
    if (std::isfinite(beats_per_minute) && beats_per_minute > 0.0)
        pending_host_tempo.store(beats_per_minute, std::memory_order_release);
}

void Plugin::consume_host_tempo() const {
    // engine lock held, anything newer waits for the next block
    const auto tempo =
            pending_host_tempo.exchange(0.0, std::memory_order_acq_rel);
    if (tempo > 0.0)
        instrument_player.set_tempo(tempo);
}

void Plugin::dispatch(const PluginMidiEvent &event) {
    // frame 0 keeps MIDI before automation, only later events need an offset
    if (event.frame == 0) {
        switch (event.kind) {
            case PluginMidiKind::note_on:
                if (event.data_2)
                    (void)instrument_player.note_on(event.data_1, event.data_2);
                else
                    (void)instrument_player.note_off(event.data_1, 0);
                break;
            case PluginMidiKind::note_off:
                (void)instrument_player.note_off(event.data_1, event.data_2);
                break;
            case PluginMidiKind::control_change:
                instrument_player.control_change(event.data_1, event.data_2);
                break;
            case PluginMidiKind::pitch_bend:
                instrument_player.pitch_bend(
                        std::clamp(event.value / 8192.0F, -1.0F, 1.0F));
                break;
            case PluginMidiKind::channel_pressure:
                instrument_player.channel_pressure(event.data_1 / 127.0F);
                break;
            case PluginMidiKind::poly_pressure:
                instrument_player.poly_pressure(
                        event.data_1, event.data_2 / 127.0F);
                break;
        }
        return;
    }
    Player::InputEvent input;
    input.sample_offset = event.frame;
    input.key = event.data_1;
    input.velocity = event.data_2;
    switch (event.kind) {
        case PluginMidiKind::note_on:
            input.kind = Player::InputKind::note_on;
            break;
        case PluginMidiKind::note_off:
            input.kind = Player::InputKind::note_off;
            break;
        case PluginMidiKind::control_change:
            input.kind = Player::InputKind::control_change;
            break;
        case PluginMidiKind::pitch_bend:
            input.kind = Player::InputKind::pitch_bend;
            input.value = std::clamp(event.value / 8192.0F, -1.0F, 1.0F);
            break;
        case PluginMidiKind::channel_pressure:
            input.kind = Player::InputKind::channel_pressure;
            input.value = event.data_1 / 127.0F;
            break;
        case PluginMidiKind::poly_pressure:
            input.kind = Player::InputKind::poly_pressure;
            input.value = event.data_2 / 127.0F;
            break;
    }
    instrument_player.enqueue_input(input);
}

bool Plugin::process(std::span<float> output_left,
        std::span<float> output_right,
        std::span<const PluginMidiEvent> midi_events,
        std::span<const PluginParameterEvent> parameter_events) {
    return process_impl(
            output_left, output_right, midi_events, parameter_events, true);
}

void Plugin::process_realtime(std::span<float> output_left,
        std::span<float> output_right,
        std::span<const PluginMidiEvent> midi_events,
        std::span<const PluginParameterEvent> parameter_events) {
    process_impl(
            output_left, output_right, midi_events, parameter_events, false);
}

bool Plugin::try_process_realtime(std::span<float> output_left,
        std::span<float> output_right,
        std::span<const PluginMidiEvent> midi_events) {
    std::unique_lock lock(engine_owner, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::fill(output_left.begin(), output_left.end(), 0.0F);
        std::fill(output_right.begin(), output_right.end(), 0.0F);
        return false;
    }
    // process_impl locks again on this thread
    process_impl(output_left, output_right, midi_events, {}, false);
    return true;
}

bool Plugin::process_impl(std::span<float> output_left,
        std::span<float> output_right,
        std::span<const PluginMidiEvent> midi_events,
        std::span<const PluginParameterEvent> parameter_events,
        const bool wait_for_streaming) {
    dsp::profile::Scope timing(dsp::profile::plugin, output_left.size());
    // live gives up and outputs silence, offline waits so nothing goes missing
    std::unique_lock lock(engine_owner, std::defer_lock);
    if (wait_for_streaming) {
        lock.lock();
    } else if (!lock.try_lock()) {
        std::fill(output_left.begin(), output_left.end(), 0.0F);
        std::fill(output_right.begin(), output_right.end(), 0.0F);
        return false;
    }
    struct AudioRecency {
        Plugin &plugin;
        std::size_t frames;
        bool live;
        ~AudioRecency() {
            if (live && frames && plugin.is_active)
                // clang-format off
                plugin.audio_service_deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(2.0 * frames /
                        plugin.processing_rate));
            // clang-format on
        }
    } recency{ *this, std::min(output_left.size(), output_right.size()),
        !wait_for_streaming };
    drain_ui_controls(wait_for_streaming);
    if (command_service)
        command_service->deliver_prepared_results(instrument_player);
    const std::size_t frames =
            std::min(output_left.size(), output_right.size());
    std::fill(output_left.begin(), output_left.end(), 0.0F);
    std::fill(output_right.begin(), output_right.end(), 0.0F);
    if (!is_active || frames == 0) {
        // empty buffers just for parameters, keep the order and repeats. not
        // the host setters, they drain the UI edits too
        for (const auto &event : parameter_events)
            (void)instrument_player.set_automated_control(
                    event.id, event.value, event.normalized);
        publish_parameters();
        return true;
    }
    bool complete = true;

    // same frame events keep their order, repeats included
    const auto ordered = event_scratch->midi.order(midi_events);
    const auto ordered_parameters =
            event_scratch->parameters.order(parameter_events);
    const bool diagnostics =
            wait_for_streaming && std::getenv("EKPHORE_UI_DIAGNOSTICS");
    std::size_t cursor = 0;
    std::size_t event_index = 0;
    std::size_t parameter_index = 0;
    while (cursor < frames) {
        while (event_index < ordered.size() &&
                ordered[event_index].frame <= cursor) {
            auto event = ordered[event_index++];
            event.frame = 0;
            dispatch(event);
        }
        while (parameter_index < ordered_parameters.size() &&
                ordered_parameters[parameter_index].frame <= cursor) {
            const auto &event = ordered_parameters[parameter_index];
            (void)instrument_player.set_automated_control(
                    event.id, event.value, event.normalized);
            ++parameter_index;
        }
        auto next_event = frames;
        if (parameter_index < ordered_parameters.size())
            next_event = std::min<std::size_t>(next_event,
                    std::min<std::size_t>(
                            ordered_parameters[parameter_index].frame, frames));
        const std::size_t block_end = std::min(
                next_event, cursor + static_cast<std::size_t>(block_limit));
        if (block_end == cursor)
            continue;
        // MIDI offsets don't split the DSP, automation still does
        while (event_index < ordered.size() &&
                ordered[event_index].frame < block_end) {
            auto event = ordered[event_index++];
            event.frame = static_cast<std::uint32_t>(event.frame - cursor);
            dispatch(event);
        }
        const auto render_started = std::chrono::steady_clock::now();
        instrument_player.render_into(render_buffer, block_end - cursor,
                static_cast<std::uint32_t>(processing_rate),
                wait_for_streaming);
        const auto &rendered = render_buffer;
        complete &= !rendered.offline_sample_data_incomplete;
        if (diagnostics) {
            const auto elapsed =
                    std::chrono::steady_clock::now() - render_started;
            if (elapsed > std::chrono::milliseconds{ 25 })
                std::cerr << "player render frames=" << (block_end - cursor)
                          << " elapsed_ms="
                          << std::chrono::duration<double, std::milli>(elapsed)
                                     .count()
                          << '\n';
        }
        std::copy(rendered.left.begin(), rendered.left.end(),
                output_left.begin() + static_cast<std::ptrdiff_t>(cursor));
        std::copy(rendered.right.begin(), rendered.right.end(),
                output_right.begin() + static_cast<std::ptrdiff_t>(cursor));
        cursor = block_end;
    }
    publish_parameters();
    return complete;
}

UiDocument Plugin::editor_document() const {
    if (load_job && !load_job->done.load(std::memory_order_acquire)) {
        auto document = load_job->document;
        const auto elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - load_job->started)
                                     .count();
        document.loading_animation = std::fmod(elapsed * .65F, 1.F);
        return document;
    }
    if (nks_loading.load(std::memory_order_acquire)) {
        std::unique_lock lock(ui_mutex);
        UiDocument document;
        if (const auto previous = read_input_document(lock)) {
            document.width = std::max(previous->width, 1);
            document.height = std::max(previous->height, 1);
        }
        document.title = "ekphore — Loading instrument";
        document.background = { 29, 26, 32, 255 };
        document.loading_instrument =
                nks_loading_name.empty() ? "Instrument" : nks_loading_name;
        const auto seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
        document.loading_animation = std::fmod(seconds * .65, 1.);
        return document;
    }
    std::unique_lock lock(ui_mutex);
    editor_resources_dirty = true;
    refresh_editor_readback(lock);
    auto document = read_input_document(lock);
    if (!document)
        return {};
    auto snapshot = capture_editor_snapshot(std::move(*document));
    lock.unlock();
    auto composed = std::move(snapshot).compose();
    materialize_ui_waveforms(composed);
    return composed;
}

Plugin::EditorSnapshot Plugin::capture_editor_snapshot(
        UiDocument document) const {
    const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - menu_last_scroll)
                                 .count();
    const auto opacity = static_cast<float>(
            .5 * std::clamp((610.0 - elapsed) / 160.0, 0.0, 1.0));
    return { std::move(document), editor_resources,
        ui_readback ? ui_readback->graph : nullptr, opened_menu,
        hovered_control, captured_control, focused_text_edit, hovered_xy,
        menu_hover_index, menu_scroll_offset, opacity,
        focused_text_edit ? text_edit_buffer : std::string{}, text_edit_caret,
        text_edit_selection_anchor };
}

UiDocument Plugin::EditorSnapshot::compose() && {
    if (resources) {
        if (document.background_picture.empty())
            document.background_picture = resources->background_picture;
        document.loading_progress = resources->loading_progress;
        for (auto &control : document.controls) {
            const auto *resource =
                    resources->find(control.script_slot, control.ksp_id);
            if (!resource || resource->waveform_zone != control.waveform_zone)
                continue;
            control.waveform_source = resource->waveform_source;
            control.waveform_first_frame = resource->waveform_first_frame;
            control.waveform_last_frame = resource->waveform_last_frame;
        }
    }
    if (graph) {
        document.native_ui_external_modules = graph->external_modules;
        document.native_ui_diagnostics = graph->diagnostics;
        for (const auto &module : graph->modules) {
            document.native_ui_product_modules.push_back(module.name);
            for (const auto &name : module.declarative_components)
                if (std::ranges::find(
                            document.native_ui_declarative_components, name) ==
                        document.native_ui_declarative_components.end())
                    document.native_ui_declarative_components.push_back(name);
            for (const auto &name : module.method_calls)
                if (std::ranges::find(document.native_ui_method_calls, name) ==
                        document.native_ui_method_calls.end())
                    document.native_ui_method_calls.push_back(name);
        }
    }
    materialize_ui_native(document);
    if (document.native_ui_component_roots.empty() &&
            !document.performance_view_tabs.empty() &&
            !document.performance_view_content_height) {
        constexpr std::int32_t classic_tab_height = 20;
        document.performance_view_content_height = document.height;
        document.height += classic_tab_height;
    }
    if (opened_menu)
        if (auto *menu = document.find(
                    opened_menu->first, opened_menu->second)) {
            menu->menu_open =
                    document.visible(*menu) && document.enabled(*menu);
            menu->menu_hover_index = menu_hover;
            menu->menu_scroll_offset = menu_scroll;
            menu->menu_scroll_opacity = menu_scroll_opacity;
        }
    if (hovered_control)
        if (auto *control = document.find(
                    hovered_control->first, hovered_control->second)) {
            control->hovered =
                    document.visible(*control) && document.enabled(*control);
            control->xy_hover_index = hovered_xy;
        }
    if (captured_control)
        if (auto *control = document.find(
                    captured_control->first, captured_control->second))
            control->pressed =
                    document.visible(*control) && document.enabled(*control);
    if (focused_text_edit)
        if (auto *control = document.find(
                    focused_text_edit->first, focused_text_edit->second)) {
            control->focused =
                    document.visible(*control) && document.enabled(*control);
            if (control->focused) {
                control->text = text;
                control->value = std::move(text);
                control->text_caret = caret;
                control->text_selection_anchor = selection_anchor;
            }
        }
    return std::move(document);
}

UiDocument Plugin::empty_editor_document() const {
    UiDocument document;
    document.width = 720;
    document.height = 420;
    document.background = { 24, 26, 29, 255 };
    document.title = "ekphore";

    const auto label = [](std::string name, std::string text,
                               const UiRectangle bounds, const UiColor color) {
        UiControl control;
        control.name = std::move(name);
        control.kind = UiControlKind::label;
        control.bounds = bounds;
        control.text = std::move(text);
        control.text_color = color;
        control.enabled = false;
        return control;
    };
    document.controls.push_back(
            label("@loader_title", "Load a Kontakt instrument",
                    { 176, 112, 368, 28 }, { 240, 240, 240, 255 }));
    document.controls.push_back(label("@loader_hint",
            "Paste or drop an .nki instrument or .nksn snapshot.",
            { 176, 144, 420, 24 }, { 164, 168, 174, 255 }));

    UiControl path_background;
    path_background.name = "$loader_path_background";
    path_background.kind = UiControlKind::button;
    path_background.bounds = { 112, 190, 496, 38 };
    path_background.enabled = false;
    document.controls.push_back(std::move(path_background));

    UiControl path;
    path.script_slot = loader_script_slot;
    path.ksp_id = loader_path_id;
    path.name = std::string(loader_path_name);
    path.kind = UiControlKind::text_edit;
    path.bounds = { 120, 197, 480, 24 };
    path.value = pending_instrument_path;
    path.text = pending_instrument_path;
    path.text_color = { 235, 235, 235, 255 };
    document.controls.push_back(std::move(path));

    UiControl button;
    button.script_slot = loader_script_slot;
    button.ksp_id = loader_button_id;
    button.name = std::string(loader_button_name);
    button.kind = UiControlKind::button;
    button.bounds = { 260, 252, 200, 42 };
    button.text = "Load Instrument";
    button.text_color = { 245, 245, 245, 255 };
    document.controls.push_back(std::move(button));

    if (!instrument_load_error.empty())
        document.controls.push_back(
                label("@loader_error", instrument_load_error,
                        { 112, 316, 496, 48 }, { 224, 104, 104, 255 }));
    return document;
}

void Plugin::prepare_parameter_publication() {
    ++nks_generation;
    if (!ui_commands)
        ui_commands = std::make_unique<PluginUiCommands>();
    if (!parameter_publication)
        parameter_publication = std::make_unique<PluginParameterPublication>();
    parameter_publication->prepare(instrument_player.script_ui_controls());
    auto input = std::make_shared<PluginInputPublication>();
    input->prepare(instrument_player);
    ui_readback = std::make_shared<const PlayerUiReadState>(input->read());
    native_ui_session = std::make_unique<PluginNativeUiSession>();
    auto initial = *ui_readback;
    native_ui_session->bind(initial);
    editor_resources = std::make_shared<const UiDocument>(
            instrument_player.capture_ui_document());
    editor_resources_dirty = false;
    input_publication = std::move(input);
}

void Plugin::publish_parameters() const {
    dsp::profile::Scope timing(dsp::profile::publication);
    // we hold the engine lock
    if (parameter_publication)
        parameter_publication->publish(instrument_player.script_ui_controls());
    if (input_publication)
        input_publication->publish(instrument_player,
                ui_commands ? ui_commands->completed_sequence.load() : 0);
}

std::optional<UiDocument> Plugin::read_input_document(
        std::unique_lock<std::recursive_mutex> &lock) const {
    if (!has_instrument)
        return empty_editor_document();
    const auto publication = input_publication;
    const auto assets = ui_assets;
    if (!publication)
        return std::nullopt;
    lock.unlock();
    UiDocument document;
    std::shared_ptr<PlayerUiReadState> readback;
    try {
        readback = std::make_shared<PlayerUiReadState>(publication->read());
    } catch (...) {
        lock.lock();
        throw;
    }
    lock.lock();
    if (publication != input_publication || assets != ui_assets)
        return std::nullopt;
    if (ui_commands)
        ui_commands->project(*readback);
    native_ui_session->bind(*readback);
    document = readback->document;
    ui_readback = std::move(readback);
    return document;
}

std::vector<PluginParameter> Plugin::parameters() const {
    return parameter_publication ? parameter_publication->read()
                                 : std::vector<PluginParameter>{};
}

nks::State Plugin::nks_state() const {
    std::scoped_lock engine(engine_owner);
    nks::State state;
    state.identity =
            (instrument_path ? instrument_path->string() : std::string{}) +
            ":" + std::to_string(nks_generation);
    state.artwork_identity =
            instrument_path ? instrument_path->string() : std::string{};
    if (nks_loading.load())
        state.status = "Busy... Loading";
    state.preset =
            instrument_path ? instrument_path->stem().string() : std::string{};
    state.name = state.preset.empty() ? "ekphore" : state.preset;
    if (instrument_player.has_instrument_model()) {
        state.instrument_loaded = true;
        state.lightguide.emplace();
        const auto colors = instrument_player.keyboard_colors();
        const auto pressed = instrument_player.keyboard_pressed();
        for (std::size_t note = 0; note < colors.size(); ++note)
            (*state.lightguide)[note] = libnks::mk3_led_value(
                    libnks::mk3_key_color(colors[note]), pressed[note]);
    }
    const auto controls = instrument_player.script_ui_controls();
    for (const auto &p : parameters()) {
        // turning off reassignment doesn't remove it from the fallback page
        if (!std::isfinite(p.value) || p.maximum <= p.minimum)
            continue;
        if (state.pages.empty() || state.pages.back().parameters.size() == 8)
            state.pages.push_back(
                    { "Controls " + std::to_string(state.pages.size() + 1),
                            {} });
        libnks::ParameterAttributes attributes;
        std::string display;
        char numeric[64];
        const auto formatted = std::to_chars(numeric, numeric + sizeof(numeric),
                p.value, std::chars_format::general, 6);
        if (formatted.ec == std::errc{})
            display.assign(numeric, formatted.ptr);
        const auto normalized = std::clamp(
                (p.value - p.minimum) / (p.maximum - p.minimum), 0.0, 1.0);
        for (const auto &c : controls)
            if (c.script_slot == p.script_slot && c.name == p.control) {
                attributes = { std::uint8_t(std::clamp(c.nks_type, 0, 255)),
                    std::uint8_t(std::clamp(c.nks_style, 0, 255)),
                    std::uint32_t(std::max(c.nks_num_values, 0)),
                    c.nks_string_values };
                // the script label, not the raw value
                if (c.kind == UiControlKind::knob) {
                    if (!c.label.empty()) {
                        display = c.label;
                        constexpr std::array<std::string_view, 7> units{ "",
                            " dB", " Hz", " %", " ms", " oc", " st" };
                        if (c.unit > 0 && std::size_t(c.unit) < units.size())
                            display += units[std::size_t(c.unit)];
                    }
                } else if (c.kind == UiControlKind::slider)
                    display = c.label;
                else if (c.kind == UiControlKind::switch_control)
                    display = c.label.empty() ? (p.value > 0 ? "On" : "Off")
                                              : c.label;
                else
                    display.clear();
                break;
            }
        state.pages.back().parameters.push_back(
                { p.id, p.name, display, normalized, attributes });
    }
    return state;
}

bool Plugin::array_file_active(Player::EngineCommandHandle command) const {
    std::scoped_lock lock(engine_owner);
    return instrument_player.engine_command_matches(command);
}

bool Plugin::advance_ui_preview_clock(std::size_t frames) {
    std::scoped_lock lock(engine_owner);
    if (is_active || !frames || !instrument_player.has_script_clock_work())
        return false;
    AudioBlock silence;
    while (frames) {
        const auto count = std::min<std::size_t>(frames, 1024);
        instrument_player.render_into(silence, count, 48000, false);
        frames -= count;
    }
    publish_parameters();
    // callbacks can change bindings behind our back, so
    // mark it dirty and let the UI do the work
    if (ui_commands)
        ui_commands->native_readback_dirty = true;
    return true;
}

std::vector<Player::ArrayFileRequest> Plugin::array_file_requests() const {
    std::scoped_lock lock(engine_owner);
    return instrument_player.array_file_requests();
}

bool Plugin::select_array_file(Player::EngineCommandHandle command,
        const std::optional<std::filesystem::path> &path) {
    std::scoped_lock lock(engine_owner);
    return instrument_player.select_array_file(command, path);
}

bool Plugin::set_parameter(const std::uint32_t id, const double value) {
    std::scoped_lock lock(engine_owner);
    consume_host_tempo();
    drain_ui_controls();
    const auto accepted = instrument_player.set_automated_control(id, value);
    publish_parameters();
    return accepted;
}

bool Plugin::set_normalized_parameter(
        const std::uint32_t id, const double value) {
    std::scoped_lock lock(engine_owner);
    consume_host_tempo();
    drain_ui_controls();
    const auto accepted =
            instrument_player.set_automated_control(id, value, true);
    publish_parameters();
    return accepted;
}

void Plugin::refresh_editor_readback(
        std::unique_lock<std::recursive_mutex> &lock) const {
    // hosts can render before anything is loaded
    if (!has_instrument || !native_ui_session || !ui_readback)
        return;
    const bool rebuild =
            native_rebuild_pending && !captured_native_ui_hit &&
            !native_hover_dispatching &&
            (!ui_commands ||
                    (input_publication &&
                            input_publication->completed_sequence.load() ==
                                    ui_commands->next_sequence - 1));
    const bool dirty =
            ui_commands && ui_commands->native_readback_dirty.exchange(false);
    if (!rebuild && !dirty)
        return;
    if (!read_input_document(lock)) {
        if (ui_commands)
            ui_commands->native_readback_dirty = true;
        return;
    }
    // read_input_document unlocks, a load can swap the session meanwhile
    if (!has_instrument || !native_ui_session || !ui_readback)
        return;
    auto job = native_ui_session->readback(*ui_readback, rebuild);
    if (!job) {
        if (rebuild)
            native_rebuild_pending = false;
        return;
    }
    lock.unlock();
    try {
        Player::evaluate_native_ui_readback(*job);
    } catch (...) {
        job.reset(); // could be the last ref to the old Lua state
        lock.lock();
        if (ui_commands)
            ui_commands->native_readback_dirty = true;
        throw;
    }
    lock.lock();
    publish_editor_readback(*job, rebuild);
    // a reload might have killed the job meanwhile, free it without the lock
    lock.unlock();
    job.reset();
    lock.lock();
}

void Plugin::publish_editor_readback(
        Player::NativeUiReadbackJob &job, const bool rebuild) const {
    // keep the registry for a new gesture even without a start callback
    if (rebuild &&
            (!native_rebuild_pending || captured_native_ui_hit ||
                    native_hover_dispatching ||
                    (ui_commands &&
                            (!input_publication ||
                                    input_publication->completed_sequence
                                                    .load() !=
                                            ui_commands->next_sequence - 1))))
        return;
    const bool materialized =
            native_ui_session && input_publication &&
            native_ui_session->publish(job,
                    input_publication->completed_sequence,
                    input_publication->native_revision,
                    ui_commands ? ui_commands->next_sequence - 1 : 0);
    if (!materialized && ui_commands)
        ui_commands->native_readback_dirty = true;
    if (rebuild && job.published()) {
        native_rebuild_pending = false;
        // even an error tree has a new registry
        ++editor_render_revision;
        native_ui_hits.clear();
        hovered_native_ui_hits.clear();
    }
}

UiFrame Plugin::render_editor(
        const float scale, const UiAssetProvider *assets) const {
    if (instrument_loading())
        return UiRenderer{}.render(editor_document(), scale);
    const auto profile_start = std::chrono::steady_clock::now();
    std::unique_lock lock(ui_mutex);
    editor_resources_dirty = true;
    refresh_editor_readback(lock);
    auto published = read_input_document(lock);
    if (!published)
        return {};
    auto snapshot = capture_editor_snapshot(std::move(*published));
    auto cache = ui_assets;
    auto reader = cache->reader;
    const auto revision = ++editor_render_revision;
    lock.unlock();
    auto document = std::move(snapshot).compose();
    materialize_ui_waveforms(document);
    const auto profile_document = std::chrono::steady_clock::now();
    PlayerAssetProvider instrument_assets(std::move(reader), cache);
    if (!assets && std::getenv("EKPHORE_UI_DIAGNOSTICS_ALL")) {
        for (const auto &control : document.controls) {
            if (!control.picture.empty())
                (void)instrument_assets.image(control.picture, scale);
        }
    }
    std::vector<NativeUiHitRegion> hits;
    auto frame = UiRenderer{ assets ? assets : &instrument_assets }.render(
            document, scale, &hits);
    lock.lock();
    if (cache == ui_assets && revision == editor_render_revision) {
        detail::gesture_trace(
                "render revision=%llu scale=%g hits=%zu captured=%lld",
                static_cast<unsigned long long>(revision), scale, hits.size(),
                captured_native_ui_hit
                        ? static_cast<long long>(*captured_native_ui_hit)
                        : -1LL);
        if (captured_native_ui_hit && *captured_native_ui_hit < hits.size()) {
            const auto &hit = hits[*captured_native_ui_hit];
            detail::gesture_trace(
                    "render-captured bounds=%d,%d,%d,%d control=%s value=%g",
                    hit.x, hit.y, hit.width, hit.height, hit.control_id.c_str(),
                    hit.value);
            for (const auto &action : hit.actions)
                detail::gesture_trace("render-captured action=%s id=%llu",
                        action.phase.c_str(),
                        static_cast<unsigned long long>(action.id));
        }
        native_ui_hits = std::move(hits);
    }
    lock.unlock();
    if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG")) {
        const auto micros = [](const auto a, const auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a)
                    .count();
        };
        std::cerr << "native-render-us document="
                  << micros(profile_start, profile_document) << " draw="
                  << micros(profile_document, std::chrono::steady_clock::now())
                  << '\n';
    }
    return frame;
}

UiRectangle Plugin::picture_pointer_bounds(const UiDocument &document,
        const UiControl &control,
        const std::shared_ptr<PluginUiAssetCache> &cache,
        const std::optional<UiRectangle> resolved) const {
    auto logical = resolved ? *resolved : document.bounds_of(control);
    // the whole rectangle, whatever the picture is
    if (control.kind == UiControlKind::xy)
        return logical;
    if (control.picture.empty())
        return logical;
    PlayerAssetProvider assets(cache->reader, cache);
    const auto image = assets.image(control.picture);
    if (!image || !image->valid())
        return logical;
    const auto frames = std::max(image->animation_count, 1U);
    const auto frame_width = static_cast<std::int32_t>(
            image->horizontal_animation ? image->width / frames : image->width);
    const auto frame_height = static_cast<std::int32_t>(
            image->horizontal_animation ? image->height
                                        : image->height / frames);
    auto picture = logical;
    const bool button = control.kind == UiControlKind::button;
    const bool anchored = button ||
                          control.kind == UiControlKind::switch_control ||
                          control.kind == UiControlKind::menu ||
                          control.kind == UiControlKind::label;
    if (button) {
        ++picture.x;
        ++picture.y;
        picture.width = std::max(picture.width - 2, 0);
        picture.height = std::max(picture.height - 2, 0);
    }
    if (control.kind == UiControlKind::slider) {
        picture.width = frame_width;
        picture.height = frame_height;
        return picture;
    }
    constexpr std::int32_t padding_limit = 4;
    if (!control.width_explicit || !image->horizontal_resizable) {
        if (!anchored && frame_width <= picture.width &&
                picture.width - frame_width <= padding_limit)
            picture.x += (picture.width - frame_width) / 2;
        picture.width = frame_width;
    }
    if (!control.height_explicit || !image->vertical_resizable) {
        if (!anchored && frame_height <= picture.height &&
                picture.height - frame_height <= padding_limit)
            picture.y += (picture.height - frame_height) / 2;
        picture.height = frame_height;
    }
    if (button)
        return picture;
    const auto left = std::min(logical.x, picture.x);
    const auto top = std::min(logical.y, picture.y);
    const auto right =
            std::max(logical.x + logical.width, picture.x + picture.width);
    const auto bottom =
            std::max(logical.y + logical.height, picture.y + picture.height);
    return { left, top, right - left, bottom - top };
}

const UiControl *Plugin::editor_hit_test(const UiDocument &document,
        const std::int32_t x, const std::int32_t y,
        std::unique_lock<std::recursive_mutex> &lock) const {
    const UiControl *found{};
    const auto cache = ui_assets;
    const auto accepts_pointer = [](const UiControlKind kind) {
        return kind == UiControlKind::button ||
               kind == UiControlKind::switch_control ||
               kind == UiControlKind::knob || kind == UiControlKind::slider ||
               kind == UiControlKind::value_edit ||
               kind == UiControlKind::menu || kind == UiControlKind::table ||
               kind == UiControlKind::xy || kind == UiControlKind::waveform ||
               kind == UiControlKind::wavetable ||
               kind == UiControlKind::file_selector ||
               kind == UiControlKind::text_edit ||
               kind == UiControlKind::mouse_area;
    };
    lock.unlock();
    try {
        const auto resolved = document.resolve_controls();
        for (const auto *ordered : document.classic_paint_order()) {
            const auto &item = resolved[static_cast<std::size_t>(
                    ordered - document.controls.data())];
            const auto &control = *item.control;
            if (!accepts_pointer(control.kind) || !item.visible ||
                    !item.enabled ||
                    !picture_pointer_bounds(
                            document, control, cache, item.bounds)
                            .contains(x, y))
                continue;
            found = &control;
        }
    } catch (...) {
        lock.lock();
        throw;
    }
    lock.lock();
    return cache == ui_assets ? found : nullptr;
}

bool Plugin::apply_pointer_value(const UiControl &control, const std::int32_t x,
        const std::int32_t y, const bool initial,
        const std::optional<std::pair<std::int32_t, std::int32_t>> previous) {
    // not a value change, drops go through the path below
    if (control.kind == UiControlKind::mouse_area)
        return false;
    // same, shaper gestures go their own way
    if (control.kind == UiControlKind::wavetable)
        return false;
    // these need a selection, not a value
    if (control.kind == UiControlKind::menu ||
            control.kind == UiControlKind::file_selector)
        return false;
    if (control.kind == UiControlKind::waveform) {
        if (initial) {
            waveform_drag.reset();
            if ((control.waveform_flags & 3) != 3 ||
                    !control.waveform_sample_frames ||
                    control.bounds.width <= 0 || control.bounds.height <= 0)
                return false;
            const double scale = double(control.bounds.width) /
                                 control.waveform_sample_frames;
            for (std::size_t i = 0;
                    i <
                    std::min<std::size_t>(control.waveform_slices.size(), 256);
                    ++i) {
                const auto [first, last] = control.waveform_slices[i];
                const auto local_x = x - control.bounds.x;
                if (local_x < static_cast<std::int64_t>(scale * first) ||
                        local_x >= static_cast<std::int64_t>(scale * last))
                    continue;
                auto target = ui_readback
                                      ? ui_readback->target(control.script_slot,
                                                control.name)
                                      : std::nullopt;
                if (!target)
                    return false;
                WaveformDrag drag{};
                drag.target = std::move(*target);
                drag.table = control.waveform_table;
                drag.table.resize(256);
                drag.index = static_cast<std::int32_t>(i);
                drag.origin_y = y;
                drag.height = control.bounds.height;
                drag.initial = float(drag.table[i]) / 1000000.0F;
                drag.minimum = (control.waveform_flags & 4) ? -1.0F : 0.0F;
                drag.range = (control.waveform_flags & 4) ? 2.0F : 1.0F;
                waveform_drag = std::move(drag);
                break;
            }
        }
        if (!waveform_drag)
            return false;
        auto &drag = *waveform_drag;
        if (!initial) {
            const float value = std::clamp(
                    drag.initial + drag.range *
                                           (float(drag.origin_y) - float(y)) /
                                           drag.height,
                    drag.minimum, 1.0F);
            drag.table[drag.index] =
                    static_cast<std::int32_t>(value * 1000000.0F);
        }
        PluginUiCommands::Command command{};
        command.target = drag.target;
        command.waveform.emplace(drag.table, drag.index);
        return ui_commands->submit(std::move(command));
    }
    std::int64_t value{};
    if (control.kind == UiControlKind::button ||
            control.kind == UiControlKind::switch_control) {
        if (!initial)
            return true;
        const auto current = std::get_if<std::int64_t>(&control.value);
        value = current && *current == 0 ? control.maximum : control.minimum;
    } else if (control.kind == UiControlKind::table) {
        const auto *current_values =
                std::get_if<std::vector<std::int64_t>>(&control.value);
        if (!current_values || current_values->empty())
            return false;
        const auto value_count = current_values->size();
        const auto point = [&](const std::int32_t px, const std::int32_t py) {
            const auto normalized_x =
                    std::clamp(static_cast<double>(px - control.bounds.x) /
                                       std::max(control.bounds.width, 1),
                            0.0, 0.999999);
            const auto index = std::min<std::size_t>(
                    static_cast<std::size_t>(normalized_x * value_count),
                    value_count - 1);
            const auto normalized_y = std::clamp(
                    1.0 - static_cast<double>(py - control.bounds.y) /
                                    std::max(control.bounds.height, 1),
                    0.0, 1.0);
            return std::pair{ index,
                static_cast<std::int64_t>(
                        std::llround(normalized_y * control.table_maximum)) };
        };
        const auto current_point = point(x, y);
        const auto prior = previous ? point(previous->first, previous->second)
                                    : current_point;
        // the callbacks don't give us a new array between cells, keep ours
        auto values = *current_values;
        values[current_point.first] = current_point.second;
        bool changed{};
        const auto publish = [&] {
            changed = submit_ui_control(
                              control.script_slot, control.name, values) ||
                      changed;
        };
        const auto distance =
                std::abs(static_cast<std::int64_t>(current_point.first) -
                         static_cast<std::int64_t>(prior.first));
        if (initial || distance <= 1) {
            publish(); // change phase
            publish(); // completion phase
            return changed;
        }
        const auto direction = current_point.first < prior.first ? 1 : -1;
        auto index = current_point.first;
        for (;;) {
            // clang-format off
            const auto fraction = prior.first == current_point.first ? 1.0 :
                    static_cast<double>(static_cast<std::int64_t>(index) -
                    static_cast<std::int64_t>(prior.first)) /
                    (static_cast<double>(current_point.first) -
                    static_cast<double>(prior.first));
            // clang-format on
            const auto value = static_cast<std::int64_t>(std::llround(
                    prior.second +
                    fraction * (current_point.second - prior.second)));
            values[index] = value;
            publish();
            if (index == prior.first)
                break;
            index = static_cast<std::size_t>(
                    static_cast<std::int64_t>(index) + direction);
        }
        publish(); // ramp phase
        publish(); // completion phase
        return changed;
    } else if (control.kind == UiControlKind::xy) {
        if (captured_xy) {
            auto &drag = *captured_xy;
            const auto travel_x = control.bounds.width - drag.bounds.width;
            const auto travel_y = control.bounds.height - drag.bounds.height;
            auto value = drag.initial;
            if (initial && control.xy_mouse_mode == 2) {
                if (travel_x > 0)
                    value[0] = std::clamp(
                            (x - control.bounds.x - drag.bounds.width * 0.5) /
                                    travel_x,
                            0.0, 1.0);
                if (travel_y > 0)
                    // clang-format off
                    value[1] = std::clamp(1.0 - (y - control.bounds.y -
                            drag.bounds.height * 0.5) / travel_y, 0.0, 1.0);
                // clang-format on
                drag.initial = value;
            } else if (!initial) {
                const auto sx = control.xy_mouse_behavior_x > 0
                                        ? control.xy_mouse_behavior_x / 1000.0
                                        : 1.0;
                const auto sy = control.xy_mouse_behavior_y > 0
                                        ? control.xy_mouse_behavior_y / 1000.0
                                        : 1.0;
                if (travel_x > 0)
                    // clang-format off
                    value[0] = std::clamp(drag.initial[0] + (x -
                            drag.origin_x) * sx / travel_x, 0.0, 1.0);
                // clang-format on
                if (travel_y > 0)
                    // clang-format off
                    value[1] = std::clamp(drag.initial[1] - (y -
                            drag.origin_y) * sy / travel_y, 0.0, 1.0);
                // clang-format on
            }
            drag.current = value;
            return submit_ui_xy(control.script_slot, control.name,
                    drag.cursor * 2, value, initial ? 1 : 3);
        }
        const auto *current_values =
                std::get_if<std::vector<double>>(&control.value);
        if (!current_values || current_values->empty())
            return false;
        const auto cursor_count = (current_values->size() + 1) / 2;
        const auto cursor = std::min<std::size_t>(
                static_cast<std::size_t>(std::max(control.xy_active_index, 0)),
                cursor_count - 1);
        const auto horizontal =
                std::clamp(static_cast<double>(x - control.bounds.x) /
                                   std::max(control.bounds.width, 1),
                        0.0, 1.0);
        // clang-format off
        const auto vertical = std::clamp(1.0 - static_cast<double>(y -
                control.bounds.y) / std::max(control.bounds.height, 1), 0.0,
                1.0);
        // clang-format on
        return submit_ui_xy(control.script_slot, control.name, cursor * 2,
                { horizontal, vertical }, initial ? 1 : 3);
    } else if ((control.kind == UiControlKind::slider ||
                       control.kind == UiControlKind::knob) &&
               initial) {
        // the value doesn't jump to the mouse on press
        return true;
    } else {
        // clang-format off
        const float normalized = control.bounds.height > control.bounds.width ?
                1.0F - static_cast<float>(y - control.bounds.y) /
                std::max(control.bounds.height, 1) : static_cast<float>(x -
                control.bounds.x) / std::max(control.bounds.width, 1);
        // clang-format on
        const double raw =
                control.minimum +
                std::clamp(normalized, 0.0F, 1.0F) *
                        static_cast<double>(control.maximum - control.minimum);
        const auto step = std::max<std::int64_t>(control.step, 1);
        value = control.minimum + static_cast<std::int64_t>(std::llround(
                                          (raw - control.minimum) / step)) *
                                          step;
        value = std::clamp(value, control.minimum, control.maximum);
    }
    if (control.kind == UiControlKind::slider ||
            control.kind == UiControlKind::knob ||
            control.kind == UiControlKind::value_edit)
        return submit_ui_control(control.script_slot, control.name, value);
    // pending values only, no callbacks
    if (control.kind == UiControlKind::button ||
            control.kind == UiControlKind::switch_control)
        return submit_ui_control(control.script_slot, control.name, value);
    return false;
}

bool Plugin::pointer_down(const std::int32_t x, const std::int32_t y) {
    nks_focus_requested.store(true);
    if (instrument_loading())
        return true;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    if (!read_input_document(lock))
        return false;
    const auto pending_native_hit = std::ranges::find_if(
            native_ui_hits.rbegin(), native_ui_hits.rend(),
            [&](const NativeUiHitRegion &hit) {
                // hover wrappers don't take clicks, pass them on
                return (hit.tap || hit.drag || !hit.control_id.empty()) &&
                       x >= hit.x && y >= hit.y && x < hit.x + hit.width &&
                       y < hit.y + hit.height;
            });
    const bool native_candidate =
            !opened_menu && pending_native_hit != native_ui_hits.rend();
    const auto pending_native_value =
            native_candidate && !pending_native_hit->control_id.empty()
                    ? PluginNativeUiSession::normalized(
                              *ui_readback, pending_native_hit->control_id)
                    : std::nullopt;
    const bool pending_native_readback =
            native_candidate && (pending_native_hit->control_id.empty() ||
                                        pending_native_value.has_value());
    // no waveforms needed for input
    auto published = pending_native_readback ? std::optional{ UiDocument{} }
                                             : read_input_document(lock);
    if (!published)
        return false;
    const auto &document = *published;
    if (opened_menu) {
        if (const auto *menu = document.find(
                    opened_menu->first, opened_menu->second)) {
            auto positioned = *menu;
            positioned.menu_scroll_offset = menu_scroll_offset;
            const auto popup = ui_menu_popup_layout(document, positioned);
            if (const auto ordinal = popup.item_at(x, y); ordinal >= 0) {
                menu_pressed_index = ordinal;
                menu_hover_index = ordinal;
                return true;
            }
            // padding is part of the popup, it doesn't close it
            if (popup.bounds.contains(x, y))
                return true;
        }
        opened_menu.reset();
        hovered_control.reset();
        hovered_xy = -1;
        return true; // close it and don't click what's under
    }
    const auto found_native = std::ranges::find_if(native_ui_hits.rbegin(),
            native_ui_hits.rend(), [&](const NativeUiHitRegion &hit) {
                return (hit.tap || hit.drag || !hit.control_id.empty()) &&
                       x >= hit.x && y >= hit.y && x < hit.x + hit.width &&
                       y < hit.y + hit.height;
            });
    if (found_native != native_ui_hits.rend()) {
        auto native = std::make_optional(*found_native);
        if (!native->control_id.empty()) {
            const auto current =
                    pending_native_readback
                            ? pending_native_value
                            : PluginNativeUiSession::normalized(
                                      *ui_readback, native->control_id);
            if (current)
                native->value =
                        native->from + *current * (native->to - native->from);
        }
        const auto instrument_identity = ui_assets;
        captured_control.reset();
        captured_native_ui_hit = static_cast<std::size_t>(
                std::distance(found_native, native_ui_hits.rend()) - 1);
        native_ui_drag_origin = { { x, y } };
        native_ui_pointer_position = { { x, y } };
        native_ui_drag_initial = native->value;
        detail::gesture_trace("down pointer=%d,%d captured=%zu "
                              "bounds=%d,%d,%d,%d control=%s value=%g drag=%d",
                x, y, *captured_native_ui_hit, native->x, native->y,
                native->width, native->height, native->control_id.c_str(),
                native->value, native->drag);
        for (const auto &item : native->actions)
            detail::gesture_trace("down action=%s id=%llu", item.phase.c_str(),
                    static_cast<unsigned long long>(item.id));
        const auto action = [&](const std::string_view phase) {
            const auto found = std::ranges::find(
                    native->actions, phase, &NativeUiModifier::Action::phase);
            return found == native->actions.end() ? 0U : found->id;
        };
        const NativeUiGestureEvent event{ static_cast<double>(x),
            static_cast<double>(y), static_cast<double>(x),
            static_cast<double>(y), 0.0, 0.0, static_cast<double>(native->x),
            static_cast<double>(native->y), static_cast<double>(native->width),
            static_cast<double>(native->height), 0 };
        if (const auto start = action("start")) {
            (void)submit_native_ui_gesture(start, event, lock);
            // another instrument might have loaded while Lua ran
            if (instrument_identity != ui_assets || !captured_native_ui_hit)
                return true;
            // closing the popup killed the hit map, don't keep the index
            if (!native->drag && native->control_id.empty() &&
                    !action("complete")) {
                captured_native_ui_hit.reset();
                native_ui_drag_origin.reset();
                native_ui_pointer_position.reset();
                return true;
            }
        }
        if (!native->drag) {
            // goes after the start action
            if (native->selection_count > 1) {
                const auto position =
                        std::clamp(static_cast<double>(x - native->x) /
                                           std::max(native->width, 1),
                                0.0, std::nextafter(1.0, 0.0));
                const auto selected = std::min(
                        static_cast<std::size_t>(
                                position *
                                static_cast<double>(native->selection_count)),
                        native->selection_count - 1U);
                return submit_native_ui_control(native->control_id,
                        static_cast<double>(selected) /
                                static_cast<double>(
                                        native->selection_count - 1U));
            }
            if (native->control_id.empty())
                return true;
            const auto span = native->to - native->from;
            const auto normalized =
                    span != 0.0 ? (native->value - native->from) / span : 0.0;
            return submit_native_ui_control(
                    native->control_id, normalized >= 0.5 ? 0.0 : 1.0);
        }
        return true;
    }
    if (!document.performance_view_tabs.empty() && y >= document.height) {
        constexpr std::int32_t tab_width = 125;
        if (x >= 2) {
            const auto index = static_cast<std::size_t>((x - 2) / tab_width);
            if (index < document.performance_view_tabs.size()) {
                captured_control.reset();
                const auto selection =
                        ui_readback
                                ? ui_readback->view(
                                          document.performance_view_tabs[index]
                                                  .first)
                                : std::nullopt;
                if (!selection)
                    return false;
                PluginUiCommands::Command command{};
                command.performance_view = *selection;
                return ui_commands->submit(std::move(command));
            }
        }
    }
    const auto *control = editor_hit_test(document, x, y, lock);
    if (focused_text_edit &&
            (!control || control->kind != UiControlKind::text_edit ||
                    control->script_slot != focused_text_edit->first ||
                    control->ksp_id != focused_text_edit->second)) {
        const bool committed = commit_text_edit(document);
        if (!control)
            return committed;
    }
    if (!control)
        return false;
    if (control->kind == UiControlKind::text_edit) {
        focus_text_edit(*control, document);
        captured_control.reset();
        return true;
    }
    if (control->kind == UiControlKind::menu &&
            !control->menu_entries.empty()) {
        captured_control.reset();
        opened_menu = { { control->script_slot, control->ksp_id } };
        hovered_control.reset();
        hovered_xy = -1;
        menu_scroll_offset = 0;
        menu_last_scroll = {};
        menu_pressed_index = -1;
        const auto popup = ui_menu_popup_layout(document, *control);
        const auto selected = std::get_if<std::int64_t>(&control->value);
        menu_hover_index = -1;
        if (selected)
            for (std::size_t ordinal = 0; ordinal < popup.entries.size();
                    ++ordinal)
                if (control->menu_entries[popup.entries[ordinal]].first ==
                        *selected) {
                    menu_hover_index = static_cast<int>(ordinal);
                    menu_scroll_offset = std::clamp(
                            ordinal * 24.0 -
                                    std::max(popup.viewport.height - 24, 0),
                            0.0, popup.maximum_scroll);
                    break;
                }
        return true;
    }
    if (!has_instrument && control->ksp_id == loader_button_id) {
        if (focused_text_edit)
            (void)commit_text_edit(document);
        if (pending_instrument_path.empty()) {
            instrument_load_error =
                    "Choose an .nki instrument or .nksn snapshot first.";
            return true;
        }
        const auto path = std::filesystem::path(pending_instrument_path);
        lock.unlock();
        (void)request_load(path);
        return true;
    }
    captured_control = { { control->script_slot, control->ksp_id } };
    last_drag_position = { { x, y } };
    if (control->kind == UiControlKind::slider ||
            control->kind == UiControlKind::knob) {
        const auto current = std::get_if<std::int64_t>(&control->value);
        const auto range = control->maximum - control->minimum;
        captured_drag_normalized =
                current && range != 0
                        ? static_cast<double>(*current - control->minimum) /
                                  range
                        : 0.0;
    } else {
        captured_drag_normalized.reset();
    }
    auto positioned = *control;
    positioned.bounds = document.bounds_of(*control);
    captured_xy.reset();
    if (positioned.kind == UiControlKind::xy) {
        const auto cache = ui_assets;
        PlayerAssetProvider assets(cache->reader, cache);
        const auto *values =
                std::get_if<std::vector<double>>(&positioned.value);
        std::optional<std::pair<std::size_t, UiRectangle>> selected;
        bool custom_cursors{};
        lock.unlock();
        if (values)
            for (std::size_t cursor = 0; cursor * 2 < values->size();
                    ++cursor) {
                const auto bounds =
                        ui_xy_cursor_bounds(positioned, cursor, &assets);
                if (!bounds)
                    continue;
                custom_cursors = true;
                if ((positioned.xy_mouse_mode == 0 && bounds->contains(x, y)) ||
                        (positioned.xy_mouse_mode != 0 &&
                                static_cast<int>(cursor) ==
                                        positioned.xy_active_index)) {
                    selected = { { cursor, *bounds } };
                    break;
                }
            }
        lock.lock();
        if (cache != ui_assets)
            return true;
        if (custom_cursors && !selected) {
            captured_control.reset();
            return false;
        }
        if (selected && values) {
            const auto first = selected->first * 2;
            const std::array<double, 2> initial{ (*values)[first],
                first + 1 < values->size() ? (*values)[first + 1] : 0.0 };
            captured_xy = XyDrag{ selected->first, selected->second, x, y,
                initial, initial };
        }
    }
    return apply_pointer_value(positioned, x, y, true);
}

void Plugin::focus_text_edit(
        const UiControl &control, const UiDocument &document) {
    const auto identity = std::pair{ control.script_slot, control.ksp_id };
    if (focused_text_edit == identity)
        return;
    if (focused_text_edit)
        (void)commit_text_edit(document);
    focused_text_edit = identity;
    if (const auto *value = std::get_if<std::string>(&control.value))
        text_edit_buffer = *value;
    else
        text_edit_buffer = control.text;
    text_edit_original = text_edit_buffer;
    text_edit_caret = text_edit_buffer.size();
    text_edit_selection_anchor.reset();
}

bool Plugin::commit_text_edit(const UiDocument &document) {
    if (!focused_text_edit)
        return false;
    const auto identity = *focused_text_edit;
    const auto *control = document.find(identity.first, identity.second);
    focused_text_edit.reset();
    text_edit_selection_anchor.reset();
    if (!control)
        return true;
    if (!has_instrument && identity.first == loader_script_slot &&
            identity.second == loader_path_id) {
        pending_instrument_path = text_edit_buffer;
        instrument_load_error.clear();
        return true;
    }
    // commit BEFORE ui_control, editing alone doesn't notify
    return submit_ui_control(control->script_slot, control->name,
            kontakt_ascii(text_edit_buffer));
}

void Plugin::cancel_text_edit() {
    text_edit_buffer = text_edit_original;
    focused_text_edit.reset();
    text_edit_selection_anchor.reset();
}

bool Plugin::text_editing_active() const {
    if (instrument_loading())
        return false;
    std::scoped_lock lock(ui_mutex);
    return focused_text_edit.has_value();
}

bool Plugin::text_input(const std::string_view utf8) {
    if (instrument_loading())
        return true;
    std::scoped_lock lock(ui_mutex);
    if (!focused_text_edit || utf8.empty())
        return false;
    // stop at the first C0 byte, UTF-8 is fine
    const auto accepted_end = std::ranges::find_if(utf8,
            [](const unsigned char character) { return character < 0x20U; });
    const std::string_view accepted(utf8.begin(), accepted_end);
    if (accepted.empty())
        return false;
    auto first = text_edit_caret;
    auto last = text_edit_caret;
    if (text_edit_selection_anchor) {
        first = std::min(first, *text_edit_selection_anchor);
        last = std::max(last, *text_edit_selection_anchor);
    }
    text_edit_buffer.replace(first, last - first, accepted);
    text_edit_caret = first + accepted.size();
    text_edit_selection_anchor.reset();
    return true;
}

bool Plugin::key_down(const UiKey key, const bool shift, const bool control) {
    if (instrument_loading())
        return true;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    if (opened_menu) {
        auto document = read_input_document(lock);
        if (!document)
            return false;
        const auto *menu =
                document->find(opened_menu->first, opened_menu->second);
        if (!menu || key == UiKey::escape) {
            opened_menu.reset();
            return true;
        }
        auto positioned = *menu;
        positioned.menu_scroll_offset = menu_scroll_offset;
        const auto popup = ui_menu_popup_layout(*document, positioned);
        if (key == UiKey::enter && menu_hover_index >= 0 &&
                menu_hover_index < static_cast<int>(popup.entries.size())) {
            const auto value =
                    menu->menu_entries[popup.entries[menu_hover_index]].first;
            opened_menu.reset();
            return submit_ui_control(menu->script_slot, menu->name, value);
        }
        const auto last = static_cast<int>(popup.entries.size()) - 1;
        if (last < 0)
            return true;
        if (key == UiKey::home)
            menu_hover_index = 0;
        else if (key == UiKey::end)
            menu_hover_index = last;
        else if (key == UiKey::up)
            menu_hover_index = std::max(menu_hover_index - 1, 0);
        else if (key == UiKey::down)
            menu_hover_index = std::min(menu_hover_index + 1, last);
        else
            return true;
        const auto top = menu_hover_index * 24.0;
        if (top < menu_scroll_offset)
            menu_scroll_offset = top;
        else if (top + 24 > menu_scroll_offset + popup.viewport.height)
            menu_scroll_offset = top + 24 - popup.viewport.height;
        menu_scroll_offset =
                std::clamp(menu_scroll_offset, 0.0, popup.maximum_scroll);
        return true;
    }
    if (!focused_text_edit)
        return false;
    (void)control;
    const auto erase_selection = [&]() {
        if (!text_edit_selection_anchor ||
                *text_edit_selection_anchor == text_edit_caret)
            return false;
        const auto first =
                std::min(*text_edit_selection_anchor, text_edit_caret);
        const auto last =
                std::max(*text_edit_selection_anchor, text_edit_caret);
        text_edit_buffer.erase(first, last - first);
        text_edit_caret = first;
        text_edit_selection_anchor.reset();
        return true;
    };
    const auto move = [&](const std::size_t position) {
        if (shift) {
            if (!text_edit_selection_anchor)
                text_edit_selection_anchor = text_edit_caret;
        } else {
            text_edit_selection_anchor.reset();
        }
        text_edit_caret = position;
    };
    switch (key) {
        case UiKey::escape:
            cancel_text_edit();
            return true;
        case UiKey::tab:
        case UiKey::enter: {
            auto document = read_input_document(lock);
            return document && commit_text_edit(*document);
        }
        case UiKey::backspace:
            if (!erase_selection() && text_edit_caret > 0) {
                const auto previous = previous_utf8_boundary(
                        text_edit_buffer, text_edit_caret);
                text_edit_buffer.erase(previous, text_edit_caret - previous);
                text_edit_caret = previous;
            }
            return true;
        case UiKey::delete_forward:
            if (!erase_selection() && text_edit_caret < text_edit_buffer.size())
                text_edit_buffer.erase(text_edit_caret,
                        next_utf8_boundary(text_edit_buffer, text_edit_caret) -
                                text_edit_caret);
            return true;
        case UiKey::home:
        case UiKey::up:
            move(0);
            return true;
        case UiKey::end:
        case UiKey::down:
            move(text_edit_buffer.size());
            return true;
        case UiKey::left:
            move(previous_utf8_boundary(text_edit_buffer, text_edit_caret));
            return true;
        case UiKey::right:
            move(next_utf8_boundary(text_edit_buffer, text_edit_caret));
            return true;
        case UiKey::select_all:
            text_edit_selection_anchor = 0;
            text_edit_caret = text_edit_buffer.size();
            return true;
    }
    return false;
}

bool Plugin::menu_open() const {
    std::scoped_lock lock(ui_mutex);
    return opened_menu.has_value();
}

bool Plugin::menu_animating() const {
    std::scoped_lock lock(ui_mutex);
    return opened_menu && std::chrono::steady_clock::now() - menu_last_scroll <
                                  std::chrono::milliseconds{ 650 };
}

bool Plugin::pointer_scroll(
        const std::int32_t x, const std::int32_t y, const double pixels) {
    if (instrument_loading())
        return true;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    if (!opened_menu)
        return false;
    auto document = read_input_document(lock);
    if (!document)
        return false;
    const auto *menu = document->find(opened_menu->first, opened_menu->second);
    if (!menu) {
        opened_menu.reset();
        return true;
    }
    auto positioned = *menu;
    positioned.menu_scroll_offset = menu_scroll_offset;
    const auto popup = ui_menu_popup_layout(*document, positioned);
    // modal menus eat the wheel even outside
    if (!popup.bounds.contains(x, y) || !std::isfinite(pixels))
        return true;
    menu_scroll_offset =
            std::clamp(popup.scroll_offset + pixels, 0.0, popup.maximum_scroll);
    if (popup.maximum_scroll > 0 && pixels != 0) {
        menu_last_scroll = std::chrono::steady_clock::now();
        menu_pressed_index = -1;
    }
    positioned.menu_scroll_offset = menu_scroll_offset;
    menu_hover_index =
            ui_menu_popup_layout(*document, positioned).item_at(x, y);
    return true;
}

bool Plugin::pointer_move(const std::int32_t x, const std::int32_t y) {
    if (instrument_loading())
        return true;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    if (opened_menu) {
        auto document = read_input_document(lock);
        if (!document)
            return false;
        const auto *menu =
                document->find(opened_menu->first, opened_menu->second);
        if (!menu) {
            opened_menu.reset();
            return true;
        }
        auto positioned = *menu;
        positioned.menu_scroll_offset = menu_scroll_offset;
        const auto popup = ui_menu_popup_layout(*document, positioned);
        const auto next = popup.item_at(x, y);
        const auto changed = next != menu_hover_index;
        menu_hover_index = next;
        return changed;
    }
    const auto instrument_identity = ui_assets;
    std::vector<std::size_t> next_native;
    // every nested hover wrapper sees the pointer, not just the deepest one
    for (std::size_t index = native_ui_hits.size(); index-- > 0;) {
        const auto &hit = native_ui_hits[index];
        const bool has_hover = std::ranges::any_of(
                hit.actions, [](const NativeUiModifier::Action &action) {
                    return action.phase == "enter" || action.phase == "exit" ||
                           action.phase == "move";
                });
        if (has_hover && x >= hit.x && y >= hit.y && x < hit.x + hit.width &&
                y < hit.y + hit.height)
            next_native.push_back(index);
    }
    // copy them BEFORE unlocking, a repaint can swap the hits while Lua runs
    std::vector<std::pair<std::uint64_t, NativeUiGestureEvent>> phases;
    const auto capture_phase = [&](const std::size_t index,
                                       const std::string_view phase) {
        if (index >= native_ui_hits.size())
            return;
        const auto &hit = native_ui_hits[index];
        const auto &actions = hit.actions;
        const auto found = std::ranges::find(
                actions, phase, &NativeUiModifier::Action::phase);
        const NativeUiGestureEvent native_event{ static_cast<double>(x),
            static_cast<double>(y), static_cast<double>(x),
            static_cast<double>(y), 0.0, 0.0, static_cast<double>(hit.x),
            static_cast<double>(hit.y), static_cast<double>(hit.width),
            static_cast<double>(hit.height), 0 };
        if (found != actions.end())
            phases.emplace_back(found->id, native_event);
    };
    for (const auto index : hovered_native_ui_hits)
        if (std::ranges::find(next_native, index) == next_native.end())
            capture_phase(index, "exit");
    for (const auto index : next_native)
        if (std::ranges::find(hovered_native_ui_hits, index) ==
                hovered_native_ui_hits.end())
            capture_phase(index, "enter");
    hovered_native_ui_hits = std::move(next_native);
    for (const auto index : hovered_native_ui_hits)
        capture_phase(index, "move");
    bool native_changed{};
    {
        // no registry swaps between hover phases
        native_hover_dispatching = true;
        struct EndHover {
            bool &dispatching;
            ~EndHover() { dispatching = false; }
        } end_hover{ native_hover_dispatching };
        for (std::size_t i = 0; i < phases.size(); ++i) {
            if (instrument_identity != ui_assets)
                return native_changed;
            native_changed = submit_native_ui_gesture(
                                     phases[i].first, phases[i].second, lock) ||
                             native_changed;
        }
    }
    if (instrument_identity != ui_assets)
        return native_changed;
    if (native_changed)
        refresh_editor_readback(lock);
    auto published = read_input_document(lock);
    if (!published)
        return native_changed;
    const auto &document = *published;
    const auto *control = editor_hit_test(document, x, y, lock);
    const auto next =
            control ? std::optional<
                              std::pair<std::size_t, std::int64_t>>{ std::pair{
                              control->script_slot, control->ksp_id } }
                    : std::nullopt;
    int next_xy = -1;
    if (control && control->kind == UiControlKind::xy) {
        auto positioned = *control;
        positioned.bounds = document.bounds_of(*control);
        const auto cache = ui_assets;
        PlayerAssetProvider assets(cache->reader, cache);
        lock.unlock();
        if (const auto *values =
                        std::get_if<std::vector<double>>(&positioned.value))
            for (std::size_t cursor = 0; cursor * 2 < values->size(); ++cursor)
                if (const auto bounds = ui_xy_cursor_bounds(
                            positioned, cursor, &assets);
                        bounds && bounds->contains(x, y)) {
                    next_xy = static_cast<int>(cursor);
                    break;
                }
        lock.lock();
        if (cache != ui_assets)
            return native_changed;
    }
    if (next == hovered_control && next_xy == hovered_xy)
        return native_changed;
    hovered_xy = next_xy;
    hovered_control = next;
    return true;
}

bool Plugin::pointer_drag(const std::int32_t x, const std::int32_t y) {
    if (instrument_loading())
        return true;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    if (opened_menu) {
        auto document = read_input_document(lock);
        if (!document)
            return true;
        if (const auto *menu = document->find(
                    opened_menu->first, opened_menu->second)) {
            auto positioned = *menu;
            positioned.menu_scroll_offset = menu_scroll_offset;
            menu_hover_index =
                    ui_menu_popup_layout(*document, positioned).item_at(x, y);
        }
        return true;
    }
    if (captured_native_ui_hit && native_ui_drag_origin &&
            *captured_native_ui_hit < native_ui_hits.size()) {
        const auto hit = native_ui_hits[*captured_native_ui_hit];
        const auto instrument_identity = ui_assets;
        const auto previous =
                native_ui_pointer_position.value_or(*native_ui_drag_origin);
        native_ui_pointer_position = { { x, y } };
        if (!hit.drag)
            return true;
        const auto delta_x = x - previous.first;
        const auto delta_y = y - previous.second;
        detail::gesture_trace("drag pointer=%d,%d previous=%d,%d delta=%d,%d "
                              "captured=%zu bounds=%d,%d,%d,%d",
                x, y, previous.first, previous.second, delta_x, delta_y,
                *captured_native_ui_hit, hit.x, hit.y, hit.width, hit.height);
        const auto update = std::ranges::find(hit.actions,
                std::string_view{ "update" }, &NativeUiModifier::Action::phase);
        if (std::getenv("EKPHORE_PROFILE_CIRCULAR_DRAG")) {
            std::cerr << "native-drag-hit=" << hit.x << ',' << hit.y << ','
                      << hit.width << ',' << hit.height
                      << " id=" << hit.control_id << " value=" << hit.value
                      << " from=" << hit.from << " to=" << hit.to << ' ';
            for (const auto &action : hit.actions)
                std::cerr << action.phase << '@' << action.id << ' ';
            std::cerr << '\n';
        }
        if (update != hit.actions.end()) {
            const NativeUiGestureEvent event{ static_cast<double>(x),
                static_cast<double>(y),
                static_cast<double>(native_ui_drag_origin->first),
                static_cast<double>(native_ui_drag_origin->second),
                static_cast<double>(delta_x), static_cast<double>(delta_y),
                static_cast<double>(hit.x), static_cast<double>(hit.y),
                static_cast<double>(hit.width), static_cast<double>(hit.height),
                0 };
            // the hit map can be gone after dispatch, don't touch it
            if (hit.control_id.empty())
                return submit_native_ui_gesture(update->id, event, lock);
            (void)submit_native_ui_gesture(update->id, event, lock);
            if (instrument_identity != ui_assets || !captured_native_ui_hit ||
                    !native_ui_drag_origin)
                return true;
        }
        // from the start point, up raises it and range_factor is the
        // sensitivity
        const auto travel = std::max(hit.height, 1);
        auto value = native_ui_drag_initial +
                     static_cast<double>(native_ui_drag_origin->second - y) /
                             static_cast<double>(travel) * hit.range_factor;
        value = std::clamp(value, hit.from, hit.to);
        if (hit.step_size > 0.0)
            value = hit.from + std::round((value - hit.from) / hit.step_size) *
                                       hit.step_size;
        const auto span = hit.to - hit.from;
        return submit_native_ui_control(
                hit.control_id, span != 0.0 ? (value - hit.from) / span : 0.0);
    }
    if (!captured_control)
        return false;
    auto published = read_input_document(lock);
    if (!published || !captured_control)
        return false;
    const auto &document = *published;
    const auto *control =
            document.find(captured_control->first, captured_control->second);
    if (!control || !document.visible(*control) ||
            !document.enabled(*control)) {
        captured_control.reset();
        last_drag_position.reset();
        captured_drag_normalized.reset();
        return true;
    }
    auto positioned = *control;
    positioned.bounds = document.bounds_of(*control);
    if ((positioned.kind == UiControlKind::slider ||
                positioned.kind == UiControlKind::knob) &&
            last_drag_position && captured_drag_normalized) {
        const auto vertical = positioned.kind == UiControlKind::knob ||
                              positioned.mouse_behaviour < 0;
        // MOUSE_BEHAVIOUR is a slider thing, knobs don't scale even though
        // it's the same field for us
        const auto drag_scale = positioned.kind == UiControlKind::knob ||
                                                positioned.mouse_behaviour == 0
                                        ? 1.0
                                        : std::abs(static_cast<double>(
                                                  positioned.mouse_behaviour)) /
                                                  2000.0;
        const auto delta = vertical ? last_drag_position->second - y
                                    : x - last_drag_position->first;
        const auto cache = ui_assets;
        UiRectangle pointer_bounds;
        lock.unlock();
        try {
            pointer_bounds = picture_pointer_bounds(document, *control, cache);
        } catch (...) {
            lock.lock();
            throw;
        }
        lock.lock();
        if (cache != ui_assets || !captured_drag_normalized)
            return true;
        const auto travel = std::max(
                (vertical ? pointer_bounds.height : pointer_bounds.width) - 1,
                1);
        const auto candidate = *captured_drag_normalized +
                               static_cast<double>(delta) * drag_scale / travel;
        const auto normalized = std::clamp(candidate, 0.0, 1.0);
        // clang-format off
        const auto raw = positioned.minimum + normalized *
                static_cast<double>(positioned.maximum - positioned.minimum);
        // clang-format on
        const auto step = std::max<std::int64_t>(positioned.step, 1);
        auto value = positioned.minimum +
                     static_cast<std::int64_t>(
                             std::llround((raw - positioned.minimum) / step)) *
                             step;
        value = std::clamp(value, positioned.minimum, positioned.maximum);
        // overshooting keeps both the old value and the unclipped accumulator
        if (candidate >= 0.0 && candidate <= 1.0) {
            captured_drag_normalized = candidate;
            last_drag_position = { { x, y } };
        }
        // keep the accumulator here, otherwise a value audio hasn't picked up
        // yet resets the next delta
        (void)submit_ui_control(positioned.script_slot, positioned.name, value);
        if (std::getenv("EKPHORE_UI_DIAGNOSTICS")) {
            auto after_published = read_input_document(lock);
            if (!after_published)
                return true;
            const auto &after_document = *after_published;
            const auto after = std::ranges::find_if(
                    after_document.controls, [&](const UiControl &candidate) {
                        return candidate.script_slot ==
                                       positioned.script_slot &&
                               candidate.name == positioned.name;
                    });
            const auto actual =
                    after != after_document.controls.end()
                            ? std::get_if<std::int64_t>(&after->value)
                            : nullptr;
            std::cerr << "ui-drag=" << positioned.name << " requested=" << value
                      << " actual=" << (actual ? std::to_string(*actual) : "?")
                      << '\n';
        }
        return true;
    }
    const auto changed =
            apply_pointer_value(positioned, x, y, false, last_drag_position);
    last_drag_position = { { x, y } };
    return changed;
}

bool Plugin::mouse_area_event(const std::int32_t x, const std::int32_t y,
        const UiMouseAreaEvent &event) {
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    auto published = read_input_document(lock);
    if (!published)
        return false;
    const auto &document = *published;
    const auto *control = editor_hit_test(document, x, y, lock);
    if (!control || control->kind != UiControlKind::mouse_area)
        return false;

    auto filtered = event;
    const auto enforce_acceptance = [](std::vector<std::string> &items,
                                            const std::int32_t maximum) {
        if (maximum <= 0 || items.size() > static_cast<std::size_t>(maximum))
            items.clear();
    };
    enforce_acceptance(filtered.audio_items, control->dnd_accept_audio);
    enforce_acceptance(filtered.midi_items, control->dnd_accept_midi);
    enforce_acceptance(filtered.array_items, control->dnd_accept_array);
    const bool accepted = !filtered.audio_items.empty() ||
                          !filtered.midi_items.empty() ||
                          !filtered.array_items.empty();

    // enter/leave only if asked, the drop always goes through
    if (filtered.type == 3 && !control->receive_drag_events)
        return accepted;
    const auto target = ui_readback ? ui_readback->target(control->script_slot,
                                              control->name)
                                    : std::nullopt;
    if (!target)
        return false;
    PluginUiCommands::Command command{};
    command.target = *target;
    command.mouse = std::move(filtered);
    const bool delivered = ui_commands->submit(std::move(command));
    return event.type == 4 ? accepted : delivered || accepted;
}

bool Plugin::drop_files(const std::int32_t x, const std::int32_t y,
        const std::span<const std::filesystem::path> files,
        const std::uint32_t modifiers) {
    if (instrument_loading())
        return false;
    std::unique_lock lock(ui_mutex);
    {
        const auto instrument =
                std::ranges::find_if(files, [this](const auto &file) {
                    auto extension = file.extension().string();
                    std::ranges::transform(extension, extension.begin(),
                            [](const unsigned char character) {
                                return static_cast<char>(
                                        std::tolower(character));
                            });
                    return extension == ".nksn" ||
                           (!has_instrument && extension == ".nki");
                });
        if (instrument != files.end()) {
            pending_instrument_path = instrument->string();
            const auto path = *instrument;
            lock.unlock();
            (void)request_load(path);
            return true;
        }
    }
    UiMouseAreaEvent event;
    event.type = 4;
    event.modifiers = modifiers;
    for (const auto &file : files) {
        auto extension = file.extension().string();
        std::ranges::transform(extension, extension.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
        if (!extension.empty() && extension.front() == '.')
            extension.erase(extension.begin());
        const auto path = file.string();
        if (extension == "mid") {
            event.midi_items.push_back(path);
        } else if (extension == "nka") {
            event.array_items.push_back(path);
        } else if (extension == "wav" || extension == "aif" ||
                   extension == "aiff" || extension == "sd2" ||
                   extension == "ncw" || extension == "rex" ||
                   extension == "rcy" || extension == "rx2" ||
                   extension == "snd") {
            event.audio_items.push_back(path);
        }
    }
    lock.unlock(); // don't keep the outer lock while decoding
    return mouse_area_event(x, y, event);
}

bool Plugin::select_file(const std::int32_t x, const std::int32_t y,
        const std::filesystem::path &path) {
    if (path.empty())
        return false;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    auto published = read_input_document(lock);
    if (!published)
        return false;
    const auto &document = *published;
    const auto *control = editor_hit_test(document, x, y, lock);
    if (!control || control->kind != UiControlKind::file_selector)
        return false;
    const auto target = ui_readback ? ui_readback->target(control->script_slot,
                                              control->name)
                                    : std::nullopt;
    if (!target)
        return false;
    PluginUiCommands::Command command{};
    command.target = *target;
    command.file_selection = path;
    return ui_commands->submit(std::move(command));
}

void Plugin::pointer_up() {
    if (instrument_loading())
        return;
    std::scoped_lock gesture(gesture_mutex);
    std::unique_lock lock(ui_mutex);
    detail::gesture_trace("up captured=%lld hits=%zu revision=%llu",
            captured_native_ui_hit
                    ? static_cast<long long>(*captured_native_ui_hit)
                    : -1LL,
            native_ui_hits.size(),
            static_cast<unsigned long long>(editor_render_revision));
    if (opened_menu && menu_pressed_index >= 0) {
        const auto pressed = std::exchange(menu_pressed_index, -1);
        auto document = read_input_document(lock);
        if (document && pressed == menu_hover_index) {
            if (const auto *menu = document->find(
                        opened_menu->first, opened_menu->second)) {
                const auto popup = ui_menu_popup_layout(*document, *menu);
                if (pressed < static_cast<int>(popup.entries.size())) {
                    const auto value =
                            menu->menu_entries[popup.entries[pressed]].first;
                    opened_menu.reset();
                    (void)submit_ui_control(
                            menu->script_slot, menu->name, value);
                }
            }
        }
        return;
    }
    if (captured_control) {
        auto document = read_input_document(lock);
        if (document) {
            const auto control = std::ranges::find_if(
                    document->controls, [&](const UiControl &item) {
                        return item.script_slot == captured_control->first &&
                               item.ksp_id == captured_control->second;
                    });
            if (control != document->controls.end() &&
                    control->kind == UiControlKind::xy) {
                if (captured_xy) {
                    (void)submit_ui_xy(control->script_slot, control->name,
                            captured_xy->cursor * 2, captured_xy->current, 2);
                } else {
                    const auto *values =
                            std::get_if<std::vector<double>>(&control->value);
                    const auto first = static_cast<std::size_t>(std::max(
                                               control->xy_active_index, 0)) *
                                       2;
                    if (values && first < values->size())
                        (void)submit_ui_xy(control->script_slot, control->name,
                                first,
                                { (*values)[first],
                                        first + 1 < values->size()
                                                ? (*values)[first + 1]
                                                : 0.0 },
                                2);
                }
            }
        }
    }
    if (captured_native_ui_hit && native_ui_drag_origin &&
            *captured_native_ui_hit < native_ui_hits.size()) {
        const auto &hit = native_ui_hits[*captured_native_ui_hit];
        const auto position =
                native_ui_pointer_position.value_or(*native_ui_drag_origin);
        const NativeUiGestureEvent event{ static_cast<double>(position.first),
            static_cast<double>(position.second),
            static_cast<double>(native_ui_drag_origin->first),
            static_cast<double>(native_ui_drag_origin->second),
            static_cast<double>(position.first - native_ui_drag_origin->first),
            static_cast<double>(
                    position.second - native_ui_drag_origin->second),
            static_cast<double>(hit.x), static_cast<double>(hit.y),
            static_cast<double>(hit.width), static_cast<double>(hit.height),
            0 };
        const auto complete =
                std::ranges::find(hit.actions, std::string_view{ "complete" },
                        &NativeUiModifier::Action::phase);
        // copy the ID now, the hit map can be gone after
        const auto complete_id =
                complete != hit.actions.end() ? complete->id : 0;
        detail::gesture_trace(
                "up-dispatch id=%llu pointer=%d,%d origin=%d,%d bounds=%d,%d,%d,%d",
                static_cast<unsigned long long>(complete_id), position.first,
                position.second, native_ui_drag_origin->first,
                native_ui_drag_origin->second, hit.x, hit.y, hit.width,
                hit.height);
        if (complete_id)
            (void)submit_native_ui_gesture(complete_id, event, lock);
        // NOT a fresh registry, that resets popups and such on every release
        if (ui_commands)
            ui_commands->native_readback_dirty = true;
        // new layout, new hit map
        ++editor_render_revision;
        native_ui_hits.clear();
        hovered_native_ui_hits.clear();
    }
    captured_control.reset();
    last_drag_position.reset();
    captured_drag_normalized.reset();
    captured_xy.reset();
    captured_native_ui_hit.reset();
    waveform_drag.reset();
    native_ui_drag_origin.reset();
    native_ui_pointer_position.reset();
    // AFTER releasing the capture
    refresh_editor_readback(lock);
}

} // namespace ekphore
