// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>
#include <ekphore/plugin.h>

#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/gui.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/render.h>
#include <clap/ext/state.h>
#include <clap/ext/timer-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr const char *features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t descriptor{ CLAP_VERSION_INIT,
    "io.github.ekphore", "ekphore", "GovanifY", "https://ekphore.github.io",
    "https://ekphore.github.io/docs", "https://github.com/ekphore/ekphore/issues",
    "0.1.0",
    "A behaviorally compatible Kontakt player replacement", features };

struct Instance {
    clap_plugin_t clap{};
    const clap_host_t *host{};
    ekphore::Plugin engine;
    // audio thread only. reserved up front, a bigger batch grows instead of
    // dropping events
    std::vector<ekphore::PluginMidiEvent> midi_events;
    std::vector<ekphore::PluginParameterEvent> parameter_events;
    bool initialized{};
    bool processing{};
    std::atomic<bool> offline{};
    std::unique_ptr<ekphore::EditorSession> editor_session;
    std::unique_ptr<ekphore::NativeEditor> editor;
    std::string editor_api;
    const clap_host_timer_support_t *host_timer{};
    clap_id editor_timer{ CLAP_INVALID_ID };
};

Instance *instance(const clap_plugin_t *plugin) {
    return static_cast<Instance *>(plugin->plugin_data);
}

bool plugin_init(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    self->engine.set_processing_request([host = self->host] {
        if (host && host->request_process)
            host->request_process(host);
    });
    self->initialized = true;
    return true;
}

void plugin_destroy(const clap_plugin_t *plugin) { delete instance(plugin); }

bool plugin_activate(const clap_plugin_t *plugin, const double sample_rate,
        const std::uint32_t, const std::uint32_t maximum_frames) {
    auto *self = instance(plugin);
    self->midi_events.reserve(1024);
    self->parameter_events.reserve(1024);
    self->engine.activate(sample_rate, maximum_frames);
    return true;
}

void plugin_deactivate(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    self->processing = false;
    self->engine.deactivate();
}

bool plugin_start_processing(const clap_plugin_t *plugin) {
    instance(plugin)->processing = true;
    return true;
}

void plugin_stop_processing(const clap_plugin_t *plugin) {
    instance(plugin)->processing = false;
}

void plugin_reset(const clap_plugin_t *plugin) {
    instance(plugin)->engine.reset();
}

void append_note_event(const clap_event_note_t &source,
        const ekphore::PluginMidiKind kind,
        std::vector<ekphore::PluginMidiEvent> &events) {
    if (source.key < 0 || source.key > 127)
        return;
    const auto velocity = static_cast<std::uint8_t>(
            std::clamp(std::lround(source.velocity * 127.0), 0L, 127L));
    events.push_back({ kind, source.header.time,
            static_cast<std::uint8_t>(std::clamp<int>(source.channel, 0, 15)),
            static_cast<std::uint8_t>(source.key), velocity, 0 });
}

void append_midi_event(const clap_event_midi_t &source,
        std::vector<ekphore::PluginMidiEvent> &events) {
    const std::uint8_t status = source.data[0];
    const std::uint8_t command = status & 0xf0U;
    const std::uint8_t channel = status & 0x0fU;
    if (command == 0x90U) {
        events.push_back(
                { source.data[2] == 0 ? ekphore::PluginMidiKind::note_off
                                      : ekphore::PluginMidiKind::note_on,
                        source.header.time, channel, source.data[1],
                        source.data[2], 0 });
    } else if (command == 0x80U) {
        events.push_back(
                { ekphore::PluginMidiKind::note_off, source.header.time,
                        channel, source.data[1], source.data[2], 0 });
    } else if (command == 0xb0U) {
        events.push_back(
                { ekphore::PluginMidiKind::control_change, source.header.time,
                        channel, source.data[1], source.data[2], 0 });
    } else if (command == 0xd0U) {
        events.push_back({ ekphore::PluginMidiKind::channel_pressure,
                source.header.time, channel, source.data[1], 0, 0 });
    } else if (command == 0xa0U) {
        events.push_back(
                { ekphore::PluginMidiKind::poly_pressure, source.header.time,
                        channel, source.data[1], source.data[2], 0 });
    } else if (command == 0xe0U) {
        const auto bend = static_cast<std::int32_t>(source.data[1]) |
                          (static_cast<std::int32_t>(source.data[2]) << 7);
        events.push_back({ ekphore::PluginMidiKind::pitch_bend,
                source.header.time, channel, 0, 0, bend - 8192 });
    }
}

clap_process_status plugin_process(
        const clap_plugin_t *plugin, const clap_process_t *process) {
    auto *self = instance(plugin);
    if (!self->processing || process->audio_outputs_count == 0 ||
            !process->audio_outputs || process->frames_count == 0)
        return CLAP_PROCESS_CONTINUE;
    auto &output = process->audio_outputs[0];
    if (!output.data32 || output.channel_count < 2 || !output.data32[0] ||
            !output.data32[1])
        return CLAP_PROCESS_ERROR;

    auto &events = self->midi_events;
    auto &parameter_events = self->parameter_events;
    events.clear();
    parameter_events.clear();
    if (process->in_events) {
        const auto count = process->in_events->size(process->in_events);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto *header =
                    process->in_events->get(process->in_events, index);
            if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID ||
                    header->time >= process->frames_count)
                continue;
            if (header->type == CLAP_EVENT_NOTE_ON)
                append_note_event(
                        *reinterpret_cast<const clap_event_note_t *>(header),
                        ekphore::PluginMidiKind::note_on, events);
            else if (header->type == CLAP_EVENT_NOTE_OFF ||
                     header->type == CLAP_EVENT_NOTE_CHOKE)
                append_note_event(
                        *reinterpret_cast<const clap_event_note_t *>(header),
                        ekphore::PluginMidiKind::note_off, events);
            else if (header->type == CLAP_EVENT_MIDI)
                append_midi_event(
                        *reinterpret_cast<const clap_event_midi_t *>(header),
                        events);
            else if (header->type == CLAP_EVENT_PARAM_VALUE) {
                const auto &event =
                        *reinterpret_cast<const clap_event_param_value_t *>(
                                header);
                parameter_events.push_back(
                        { header->time, event.param_id, event.value });
            }
        }
    }
    if (process->transport &&
            (process->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0)
        self->engine.set_tempo(process->transport->tempo);
    if (self->offline.load(std::memory_order_relaxed)) {
        if (!self->engine.process({ output.data32[0], process->frames_count },
                    { output.data32[1], process->frames_count }, events,
                    parameter_events))
            return CLAP_PROCESS_ERROR;
    } else
        self->engine.process_realtime(
                { output.data32[0], process->frames_count },
                { output.data32[1], process->frames_count }, events,
                parameter_events);
    return CLAP_PROCESS_CONTINUE;
}

std::uint32_t audio_port_count(const clap_plugin_t *, const bool is_input) {
    return is_input ? 0U : 1U;
}

bool audio_port_get(const clap_plugin_t *, const std::uint32_t index,
        const bool is_input, clap_audio_port_info_t *info) {
    if (is_input || index != 0 || !info)
        return false;
    info->id = 0;
    std::strncpy(info->name, "Stereo Output", sizeof(info->name));
    info->name[sizeof(info->name) - 1] = '\0';
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t audio_ports{ audio_port_count, audio_port_get };

std::uint32_t note_port_count(const clap_plugin_t *, const bool is_input) {
    return is_input ? 1U : 0U;
}

bool note_port_get(const clap_plugin_t *, const std::uint32_t index,
        const bool is_input, clap_note_port_info_t *info) {
    if (!is_input || index != 0 || !info)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::strncpy(info->name, "Note Input", sizeof(info->name));
    info->name[sizeof(info->name) - 1] = '\0';
    return true;
}

const clap_plugin_note_ports_t note_ports{ note_port_count, note_port_get };

std::uint32_t parameter_count(const clap_plugin_t *plugin) {
    return static_cast<std::uint32_t>(
            instance(plugin)->engine.parameters().size());
}

bool parameter_info(const clap_plugin_t *plugin, const std::uint32_t index,
        clap_param_info_t *info) {
    if (!info)
        return false;
    const auto parameters = instance(plugin)->engine.parameters();
    if (index >= parameters.size())
        return false;
    const auto &parameter = parameters[index];
    *info = {};
    info->id = parameter.id;
    info->flags = CLAP_PARAM_REQUIRES_PROCESS;
    if (!parameter.element)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (parameter.automatable)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    std::strncpy(info->name, parameter.name.c_str(), sizeof(info->name) - 1);
    info->min_value = parameter.minimum;
    info->max_value = parameter.maximum;
    info->default_value = parameter.default_value;
    return true;
}

bool parameter_value(
        const clap_plugin_t *plugin, const clap_id id, double *value) {
    if (!value)
        return false;
    const auto parameters = instance(plugin)->engine.parameters();
    const auto parameter =
            std::ranges::find(parameters, id, &ekphore::PluginParameter::id);
    if (parameter == parameters.end())
        return false;
    *value = parameter->value;
    return true;
}

bool parameter_value_to_text(const clap_plugin_t *plugin, const clap_id id,
        const double value, char *output, const std::uint32_t capacity) {
    if (!output || capacity == 0)
        return false;
    const auto parameters = instance(plugin)->engine.parameters();
    const auto parameter =
            std::ranges::find(parameters, id, &ekphore::PluginParameter::id);
    if (parameter == parameters.end())
        return false;
    const auto bounded =
            std::clamp(value, parameter->minimum, parameter->maximum);
    const auto written =
            parameter->element
                    ? std::snprintf(output, capacity, "%.6g", bounded)
                    : std::snprintf(output, capacity, "%lld",
                              static_cast<long long>(std::llround(bounded)));
    return written >= 0 && static_cast<std::uint32_t>(written) < capacity;
}

bool parameter_text_to_value(const clap_plugin_t *plugin, const clap_id id,
        const char *text, double *value) {
    if (!text || !value)
        return false;
    const auto parameters = instance(plugin)->engine.parameters();
    const auto parameter =
            std::ranges::find(parameters, id, &ekphore::PluginParameter::id);
    if (parameter == parameters.end())
        return false;
    char *end{};
    const auto parsed = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed))
        return false;
    *value = std::clamp(parsed, parameter->minimum, parameter->maximum);
    return true;
}

void parameter_flush(const clap_plugin_t *plugin,
        const clap_input_events_t *input, const clap_output_events_t *) {
    if (!input)
        return;
    auto &engine = instance(plugin)->engine;
    std::array<ekphore::PluginParameterEvent, 64> batch{};
    std::size_t used{};
    const auto count = input->size(input);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto *header = input->get(input, index);
        if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID ||
                header->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        const auto &event =
                *reinterpret_cast<const clap_event_param_value_t *>(header);
        batch[used++] = { 0, event.param_id, event.value, false };
        if (used == batch.size()) {
            engine.process_realtime({}, {}, {}, std::span{ batch }.first(used));
            used = 0;
        }
    }
    if (used)
        engine.process_realtime({}, {}, {}, std::span{ batch }.first(used));
}

const clap_plugin_params_t parameters{ parameter_count, parameter_info,
    parameter_value, parameter_value_to_text, parameter_text_to_value,
    parameter_flush };

bool write_all(
        const clap_ostream_t *stream, const void *bytes, std::uint64_t size) {
    const auto *cursor = static_cast<const std::uint8_t *>(bytes);
    while (size != 0) {
        const auto written = stream->write(stream, cursor, size);
        if (written <= 0)
            return false;
        cursor += written;
        size -= static_cast<std::uint64_t>(written);
    }
    return true;
}

bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    if (!stream || !stream->write)
        return false;
    const auto bytes = instance(plugin)->engine.save_state();
    return write_all(stream, bytes.data(), bytes.size());
}

bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    if (!stream || !stream->read)
        return false;
    constexpr std::size_t maximum_state_size = 256U * 1024U * 1024U;
    std::vector<std::byte> bytes;
    std::byte chunk[16384];
    while (true) {
        const auto count = stream->read(stream, chunk, sizeof(chunk));
        if (count < 0)
            return false;
        if (count == 0)
            break;
        if (bytes.size() + static_cast<std::size_t>(count) > maximum_state_size)
            return false;
        bytes.insert(bytes.end(), chunk, chunk + count);
    }
    return instance(plugin)->engine.load_state(bytes);
}

const clap_plugin_state_t state{ state_save, state_load };

std::optional<ekphore::NativeWindowApi> window_api(const char *api) {
    if (!api)
        return std::nullopt;
    if (!std::strcmp(api, CLAP_WINDOW_API_COCOA))
        return ekphore::NativeWindowApi::cocoa;
    if (!std::strcmp(api, CLAP_WINDOW_API_WIN32))
        return ekphore::NativeWindowApi::win32;
    if (!std::strcmp(api, CLAP_WINDOW_API_X11))
        return ekphore::NativeWindowApi::x11;
    return std::nullopt;
}

bool gui_api_supported(
        const clap_plugin_t *, const char *api, const bool floating) {
    const auto native = window_api(api);
    return !floating && native && ekphore::NativeEditor::supports(*native);
}

bool gui_preferred_api(
        const clap_plugin_t *, const char **api, bool *floating) {
    if (!api || !floating)
        return false;
#if defined(__APPLE__)
    *api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    *api = CLAP_WINDOW_API_WIN32;
#else
    *api = CLAP_WINDOW_API_X11;
#endif
    *floating = false;
    return true;
}

bool gui_create(
        const clap_plugin_t *plugin, const char *api, const bool floating) {
    auto *self = instance(plugin);
    const auto native = window_api(api);
    if (floating || !native || !ekphore::NativeEditor::supports(*native) ||
            self->editor)
        return false;
    self->editor_session =
            std::make_unique<ekphore::EditorSession>(self->engine);
    self->editor =
            std::make_unique<ekphore::NativeEditor>(*self->editor_session);
    self->editor_api = api;
    if (self->host && self->host->get_extension) {
        self->host_timer = static_cast<const clap_host_timer_support_t *>(
                self->host->get_extension(self->host, CLAP_EXT_TIMER_SUPPORT));
        if (self->host_timer && self->host_timer->register_timer)
            (void)self->host_timer->register_timer(
                    self->host, 16, &self->editor_timer);
    }
    return true;
}

void gui_destroy(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    if (self->editor_timer != CLAP_INVALID_ID && self->host_timer &&
            self->host_timer->unregister_timer)
        (void)self->host_timer->unregister_timer(
                self->host, self->editor_timer);
    self->editor_timer = CLAP_INVALID_ID;
    self->host_timer = nullptr;
    self->editor.reset();
    self->editor_session.reset();
    self->editor_api.clear();
}

bool gui_set_scale(const clap_plugin_t *plugin, const double scale) {
    auto *self = instance(plugin);
    return self->editor && std::isfinite(scale) &&
           self->editor->set_scale(static_cast<float>(scale));
}

bool gui_get_size(const clap_plugin_t *plugin, std::uint32_t *width,
        std::uint32_t *height) {
    auto *self = instance(plugin);
    if (!self->editor || !width || !height)
        return false;
    const auto size = self->editor->size();
    *width = size.first;
    *height = size.second;
    return true;
}

bool gui_can_resize(const clap_plugin_t *) { return false; }
bool gui_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *) {
    return false;
}
bool gui_adjust_size(const clap_plugin_t *plugin, std::uint32_t *width,
        std::uint32_t *height) {
    return gui_get_size(plugin, width, height);
}
bool gui_set_size(const clap_plugin_t *plugin, const std::uint32_t width,
        const std::uint32_t height) {
    std::uint32_t expected_width{};
    std::uint32_t expected_height{};
    return gui_get_size(plugin, &expected_width, &expected_height) &&
           width == expected_width && height == expected_height;
}

bool gui_set_parent(const clap_plugin_t *plugin, const clap_window_t *window) {
    auto *self = instance(plugin);
    if (!self->editor || !window || !window->api ||
            self->editor_api != window->api)
        return false;
    const auto native = window_api(window->api);
    if (!native)
        return false;
    void *handle{};
    if (*native == ekphore::NativeWindowApi::cocoa)
        handle = window->cocoa;
    else if (*native == ekphore::NativeWindowApi::win32)
        handle = window->win32;
    else
        handle = reinterpret_cast<void *>(window->x11);
    return self->editor->attach({ *native, handle, nullptr });
}

bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) {
    return false;
}
void gui_suggest_title(const clap_plugin_t *, const char *) {}
bool gui_show(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    if (!self->editor || !self->editor->attached())
        return false;
    self->editor->show();
    return true;
}
bool gui_hide(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    if (!self->editor || !self->editor->attached())
        return false;
    self->editor->hide();
    return true;
}

const clap_plugin_gui_t gui{ gui_api_supported, gui_preferred_api, gui_create,
    gui_destroy, gui_set_scale, gui_get_size, gui_can_resize, gui_resize_hints,
    gui_adjust_size, gui_set_size, gui_set_parent, gui_set_transient,
    gui_suggest_title, gui_show, gui_hide };

void timer_tick(const clap_plugin_t *plugin, const clap_id timer_id) {
    auto *self = instance(plugin);
    if (self->editor && timer_id == self->editor_timer)
        self->editor->idle();
}
const clap_plugin_timer_support_t timer_support{ timer_tick };

const clap_plugin_render_t render_mode{ [](const clap_plugin_t *) {
                                           return false;
                                       },
    [](const clap_plugin_t *plugin, clap_plugin_render_mode mode) {
        if (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE)
            return false;
        instance(plugin)->offline.store(
                mode == CLAP_RENDER_OFFLINE, std::memory_order_relaxed);
        return true;
    } };

const void *plugin_extension(const clap_plugin_t *, const char *id) {
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audio_ports;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &note_ports;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &parameters;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &state;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &gui;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &timer_support;
    if (!std::strcmp(id, CLAP_EXT_RENDER))
        return &render_mode;
    return nullptr;
}

void plugin_main_thread(const clap_plugin_t *plugin) {
    auto *self = instance(plugin);
    if (self->editor)
        self->editor->idle();
}

const clap_plugin_t *create_plugin(const clap_host_t *host) {
    auto self = std::make_unique<Instance>();
    self->host = host;
    self->clap = { &descriptor, self.get(), plugin_init, plugin_destroy,
        plugin_activate, plugin_deactivate, plugin_start_processing,
        plugin_stop_processing, plugin_reset, plugin_process, plugin_extension,
        plugin_main_thread };
    const auto *result = &self->clap;
    self.release();
    return result;
}

std::uint32_t factory_count(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factory_descriptor(
        const clap_plugin_factory_t *, const std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}

const clap_plugin_t *factory_create(const clap_plugin_factory_t *,
        const clap_host_t *host, const char *id) {
    if (!host || !id || !clap_version_is_compatible(host->clap_version) ||
            std::strcmp(id, descriptor.id))
        return nullptr;
    return create_plugin(host);
}

const clap_plugin_factory_t factory{ factory_count, factory_descriptor,
    factory_create };

bool entry_init(const char *) { return true; }
void entry_deinit() {}
const void *entry_factory(const char *id) {
    return id && !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{ CLAP_VERSION_INIT,
    entry_init, entry_deinit, entry_factory };
