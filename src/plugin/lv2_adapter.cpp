// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>
#include <ekphore/plugin.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace {

constexpr char plugin_uri[] = "https://ekphore.github.io/plugins/ekphore";
constexpr char state_uri[] = "https://ekphore.github.io/plugins/ekphore#state";
constexpr char ui_uri[] = "https://ekphore.github.io/plugins/ekphore#ui";
constexpr std::size_t maximum_state_size = 256U * 1024U * 1024U;

enum Port : std::uint32_t {
    midi_input,
    output_left,
    output_right,
    freewheel,
    offline_error,
};

struct Instance {
    ekphore::Plugin engine;
    // audio thread only, a bigger batch grows instead of dropping events
    std::vector<ekphore::PluginMidiEvent> midi_events;
    const LV2_Atom_Sequence *midi{};
    float *left{};
    float *right{};
    const float *freewheel{};
    float *offline_error{};
    bool offline_failed{};
    LV2_URID_Map *map{};
    LV2_URID midi_event{};
    LV2_URID atom_chunk{};
    LV2_URID state_key{};
    double sample_rate{ 44100.0 };
    bool active{};
};

void append_midi(const std::uint32_t frame,
        const std::span<const std::uint8_t> bytes,
        std::vector<ekphore::PluginMidiEvent> &events) {
    if (bytes.empty())
        return;
    const auto status = bytes[0];
    const auto command = static_cast<std::uint8_t>(status & 0xf0U);
    const auto channel = static_cast<std::uint8_t>(status & 0x0fU);
    const auto data = [&](const std::size_t index) {
        return index < bytes.size() ? bytes[index] : std::uint8_t{};
    };
    switch (command) {
        case LV2_MIDI_MSG_NOTE_ON:
            events.push_back({ data(2) == 0 ? ekphore::PluginMidiKind::note_off
                                            : ekphore::PluginMidiKind::note_on,
                    frame, channel, data(1), data(2), 0 });
            break;
        case LV2_MIDI_MSG_NOTE_OFF:
            events.push_back({ ekphore::PluginMidiKind::note_off, frame,
                    channel, data(1), data(2), 0 });
            break;
        case LV2_MIDI_MSG_CONTROLLER:
            events.push_back({ ekphore::PluginMidiKind::control_change, frame,
                    channel, data(1), data(2), 0 });
            break;
        case LV2_MIDI_MSG_NOTE_PRESSURE:
            events.push_back({ ekphore::PluginMidiKind::poly_pressure, frame,
                    channel, data(1), data(2), 0 });
            break;
        case LV2_MIDI_MSG_CHANNEL_PRESSURE:
            events.push_back({ ekphore::PluginMidiKind::channel_pressure, frame,
                    channel, data(1), 0, 0 });
            break;
        case LV2_MIDI_MSG_BENDER: {
            const auto value = static_cast<std::int32_t>(data(1)) |
                               (static_cast<std::int32_t>(data(2)) << 7);
            events.push_back({ ekphore::PluginMidiKind::pitch_bend, frame,
                    channel, 0, 0, value - 8192 });
            break;
        }
        default:
            break;
    }
}

LV2_Handle instantiate(const LV2_Descriptor *, const double sample_rate,
        const char *, const LV2_Feature *const *features) {
    auto self = std::make_unique<Instance>();
    self->midi_events.reserve(1024);
    self->sample_rate = sample_rate;
    if (features) {
        for (auto feature = features; *feature; ++feature) {
            if ((*feature)->URI && !std::strcmp((*feature)->URI, LV2_URID__map))
                self->map = static_cast<LV2_URID_Map *>((*feature)->data);
        }
    }
    if (!self->map || !self->map->map)
        return nullptr;
    self->midi_event = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    self->atom_chunk = self->map->map(self->map->handle, LV2_ATOM__Chunk);
    self->state_key = self->map->map(self->map->handle, state_uri);
    if (!self->midi_event || !self->atom_chunk || !self->state_key)
        return nullptr;
    return self.release();
}

void connect_port(LV2_Handle handle, const std::uint32_t port, void *data) {
    auto &self = *static_cast<Instance *>(handle);
    switch (port) {
        case midi_input:
            self.midi = static_cast<const LV2_Atom_Sequence *>(data);
            break;
        case output_left:
            self.left = static_cast<float *>(data);
            break;
        case output_right:
            self.right = static_cast<float *>(data);
            break;
        case freewheel:
            self.freewheel = static_cast<const float *>(data);
            break;
        case offline_error:
            self.offline_error = static_cast<float *>(data);
            break;
        default:
            break;
    }
}

void activate(LV2_Handle handle) {
    auto &self = *static_cast<Instance *>(handle);
    self.engine.activate(self.sample_rate, 4096);
    self.active = true;
    self.offline_failed = false;
}

void run(LV2_Handle handle, const std::uint32_t sample_count) {
    auto &self = *static_cast<Instance *>(handle);
    // latched until the next activation, the host might not check every
    // block. don't log or throw from here
    if (self.offline_error)
        *self.offline_error = self.offline_failed ? 1.0F : 0.0F;
    if (!self.left || !self.right || sample_count == 0)
        return;
    auto &events = self.midi_events;
    events.clear();
    if (self.midi) {
        LV2_ATOM_SEQUENCE_FOREACH(self.midi, event) {
            if (event->body.type != self.midi_event || event->time.frames < 0)
                continue;
            const auto frame = static_cast<std::uint32_t>(event->time.frames);
            if (frame >= sample_count)
                continue;
            const auto *body = static_cast<const std::uint8_t *>(
                    LV2_ATOM_BODY_CONST(&event->body));
            append_midi(frame, { body, event->body.size }, events);
        }
    }
    if (self.freewheel && *self.freewheel > 0.0F) {
        if (!self.engine.process({ self.left, sample_count },
                    { self.right, sample_count }, events))
            self.offline_failed = true;
    } else
        self.engine.process_realtime({ self.left, sample_count },
                { self.right, sample_count }, events);
    if (self.offline_error)
        *self.offline_error = self.offline_failed ? 1.0F : 0.0F;
}

void deactivate(LV2_Handle handle) {
    auto &self = *static_cast<Instance *>(handle);
    self.active = false;
    self.engine.deactivate();
}

void cleanup(LV2_Handle handle) { delete static_cast<Instance *>(handle); }

LV2_State_Status save_state(LV2_Handle handle, LV2_State_Store_Function store,
        LV2_State_Handle state_handle, std::uint32_t,
        const LV2_Feature *const *) {
    if (!handle || !store)
        return LV2_STATE_ERR_UNKNOWN;
    auto &self = *static_cast<Instance *>(handle);
    const auto bytes = self.engine.save_state();
    if (bytes.empty())
        return LV2_STATE_ERR_UNKNOWN;
    return store(state_handle, self.state_key, bytes.data(), bytes.size(),
            self.atom_chunk, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restore_state(LV2_Handle handle,
        LV2_State_Retrieve_Function retrieve, LV2_State_Handle state_handle,
        std::uint32_t, const LV2_Feature *const *) {
    if (!handle || !retrieve)
        return LV2_STATE_ERR_UNKNOWN;
    auto &self = *static_cast<Instance *>(handle);
    std::size_t size{};
    std::uint32_t type{};
    std::uint32_t flags{};
    const auto *value =
            retrieve(state_handle, self.state_key, &size, &type, &flags);
    if (!value)
        return LV2_STATE_SUCCESS;
    if (type != self.atom_chunk)
        return LV2_STATE_ERR_BAD_TYPE;
    if ((flags & LV2_STATE_IS_POD) == 0 || size == 0 ||
            size > maximum_state_size)
        return LV2_STATE_ERR_BAD_FLAGS;
    const auto bytes = std::span(static_cast<const std::byte *>(value), size);
    return self.engine.load_state(bytes) ? LV2_STATE_SUCCESS
                                         : LV2_STATE_ERR_UNKNOWN;
}

const LV2_State_Interface state_interface{ save_state, restore_state };

const void *extension_data(const char *uri) {
    return uri && !std::strcmp(uri, LV2_STATE__interface) ? &state_interface
                                                          : nullptr;
}

const LV2_Descriptor descriptor{ plugin_uri, instantiate, connect_port,
    activate, run, deactivate, cleanup, extension_data };

struct UiInstance {
    explicit UiInstance(Instance &plugin)
        : session(plugin.engine), editor(session) {}
    ekphore::EditorSession session;
    ekphore::NativeEditor editor;
};

ekphore::NativeWindowApi native_ui_api() {
#if defined(__APPLE__)
    return ekphore::NativeWindowApi::cocoa;
#elif defined(_WIN32)
    return ekphore::NativeWindowApi::win32;
#else
    return ekphore::NativeWindowApi::x11;
#endif
}

LV2UI_Handle ui_instantiate(const LV2UI_Descriptor *, const char *target_uri,
        const char *, LV2UI_Write_Function, LV2UI_Controller,
        LV2UI_Widget *widget, const LV2_Feature *const *features) {
    if (!target_uri || std::strcmp(target_uri, plugin_uri) || !widget ||
            !features)
        return nullptr;
    Instance *plugin{};
    void *parent{};
    for (auto feature = features; *feature; ++feature) {
        if (!std::strcmp((*feature)->URI, LV2_INSTANCE_ACCESS_URI))
            plugin = static_cast<Instance *>((*feature)->data);
        else if (!std::strcmp((*feature)->URI, LV2_UI__parent))
            parent = (*feature)->data;
    }
    const auto api = native_ui_api();
    if (!plugin || !parent || !ekphore::NativeEditor::supports(api))
        return nullptr;
    auto self = std::make_unique<UiInstance>(*plugin);
    if (!self->editor.attach({ api, parent, nullptr }))
        return nullptr;
    *widget = self->editor.native_handle();
    self->editor.show();
    return self.release();
}

void ui_cleanup(LV2UI_Handle handle) {
    delete static_cast<UiInstance *>(handle);
}

void ui_port_event(LV2UI_Handle handle, std::uint32_t, std::uint32_t,
        std::uint32_t, const void *) {
    if (handle)
        static_cast<UiInstance *>(handle)->session.invalidate();
}

int ui_idle(LV2UI_Handle handle) {
    if (!handle)
        return 1;
    static_cast<UiInstance *>(handle)->editor.idle();
    return 0;
}

const LV2UI_Idle_Interface ui_idle_interface{ ui_idle };
const void *ui_extension_data(const char *uri) {
    return uri && !std::strcmp(uri, LV2_UI__idleInterface) ? &ui_idle_interface
                                                           : nullptr;
}
const LV2UI_Descriptor ui_descriptor{ ui_uri, ui_instantiate, ui_cleanup,
    ui_port_event, ui_extension_data };

} // namespace

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(const std::uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}

LV2_SYMBOL_EXPORT
const LV2UI_Descriptor *lv2ui_descriptor(const std::uint32_t index) {
    return index == 0 ? &ui_descriptor : nullptr;
}
