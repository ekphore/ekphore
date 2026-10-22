// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>
#include <ekphore/plugin.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ekphore::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr std::size_t maximum_state_size = 256U * 1024U * 1024U;
const FUID processor_id{ 0xE17B6A31, 0x751D4FC8, 0xB2E3AAB1, 0x6A1BC149 };
const FUID controller_id{ 0xD9B1DF75, 0xB64A45DE, 0x82DBD4AE, 0xBCB3780A };
constexpr FIDString parameter_message_id = "EkphoreParameterSnapshot";
constexpr IAttributeList::AttrID parameter_data_id = "Data";
constexpr FIDString editor_state_message_id = "EkphoreEditorState";
constexpr IAttributeList::AttrID editor_state_data_id = "State";
constexpr std::uint32_t parameter_magic = 0x52504b45U; // EKPR
constexpr std::uint32_t parameter_version = 1;
constexpr std::uint32_t script_parameter_count = 2048;
constexpr std::uint32_t instrument_parameter_count = 2049;

bool trace_state_exchange() {
    static const bool enabled = std::getenv("EKPHORE_VST3_TRACE") != nullptr;
    return enabled;
}

std::string inactive_parameter_name(const std::uint32_t id) {
    if (id == script_parameter_count)
        return "NIKT0008";
    auto number = std::to_string(id);
    if (number.size() < 3)
        number.insert(0, 3 - number.size(), '0');
    return '#' + number;
}

double normalized(const PluginParameter &parameter, const double value) {
    const auto extent = parameter.maximum - parameter.minimum;
    return extent > 0.0
                   ? std::clamp((value - parameter.minimum) / extent, 0.0, 1.0)
                   : 0.0;
}

void append_u32(std::vector<std::byte> &bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::byte>(value >> shift));
}

void append_double(std::vector<std::byte> &bytes, const double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    append_u32(bytes, static_cast<std::uint32_t>(bits));
    append_u32(bytes, static_cast<std::uint32_t>(bits >> 32));
}

void append_string(std::vector<std::byte> &bytes, const std::string &value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    const auto *begin = reinterpret_cast<const std::byte *>(value.data());
    bytes.insert(bytes.end(), begin, begin + value.size());
}

std::vector<std::byte> encode_parameters(
        const std::vector<PluginParameter> &parameters) {
    std::vector<std::byte> bytes;
    append_u32(bytes, parameter_magic);
    append_u32(bytes, parameter_version);
    append_u32(bytes, static_cast<std::uint32_t>(parameters.size()));
    for (const auto &parameter : parameters) {
        append_u32(bytes, parameter.id);
        append_u32(bytes, parameter.element ? 1U : 0U);
        append_u32(bytes, parameter.automatable ? 1U : 0U);
        append_double(bytes, parameter.minimum);
        append_double(bytes, parameter.maximum);
        append_double(bytes, parameter.default_value);
        append_double(bytes, parameter.value);
        append_string(bytes, parameter.name);
    }
    return bytes;
}

class ParameterReader {
  public:
    explicit ParameterReader(const std::span<const std::byte> bytes)
        : remaining_bytes(bytes) {}

    bool u32(std::uint32_t &value) {
        if (remaining_bytes.size() < 4)
            return false;
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
            value |= std::to_integer<std::uint32_t>(remaining_bytes[shift / 8])
                     << shift;
        remaining_bytes = remaining_bytes.subspan(4);
        return true;
    }

    bool real(double &value) {
        std::uint32_t low{};
        std::uint32_t high{};
        if (!u32(low) || !u32(high))
            return false;
        value = std::bit_cast<double>(static_cast<std::uint64_t>(low) |
                                      (static_cast<std::uint64_t>(high) << 32));
        return std::isfinite(value);
    }

    bool string(std::string &value) {
        std::uint32_t size{};
        if (!u32(size) || size > remaining_bytes.size() || size > 4096)
            return false;
        value.assign(
                reinterpret_cast<const char *>(remaining_bytes.data()), size);
        remaining_bytes = remaining_bytes.subspan(size);
        return true;
    }

    [[nodiscard]] bool empty() const { return remaining_bytes.empty(); }

  private:
    std::span<const std::byte> remaining_bytes;
};

bool decode_parameters(const std::span<const std::byte> bytes,
        std::vector<PluginParameter> &parameters) {
    ParameterReader reader(bytes);
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t count{};
    if (!reader.u32(magic) || magic != parameter_magic ||
            !reader.u32(version) || version != parameter_version ||
            !reader.u32(count) || count > 2048)
        return false;
    parameters.clear();
    parameters.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        PluginParameter parameter;
        std::uint32_t has_element{};
        std::uint32_t automatable{};
        if (!reader.u32(parameter.id) || !reader.u32(has_element) ||
                has_element > 1 || !reader.u32(automatable) ||
                automatable > 1 || !reader.real(parameter.minimum) ||
                !reader.real(parameter.maximum) ||
                !reader.real(parameter.default_value) ||
                !reader.real(parameter.value) ||
                !reader.string(parameter.name) ||
                parameter.minimum > parameter.maximum)
            return false;
        if (has_element)
            parameter.element = 0;
        parameter.automatable = automatable != 0;
        parameters.push_back(std::move(parameter));
    }
    return reader.empty();
}

bool write_all(IBStream *stream, std::span<const std::byte> bytes) {
    if (!stream)
        return false;
    while (!bytes.empty()) {
        const auto request = static_cast<int32>(std::min<std::size_t>(
                bytes.size(),
                static_cast<std::size_t>(std::numeric_limits<int32>::max())));
        int32 written{};
        if (stream->write(const_cast<std::byte *>(bytes.data()), request,
                    &written) != kResultOk ||
                written <= 0 || written > request)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

bool read_all(IBStream *stream, std::vector<std::byte> &bytes) {
    if (!stream)
        return false;
    std::array<std::byte, 16384> chunk{};
    while (true) {
        int32 read{};
        const auto result = stream->read(
                chunk.data(), static_cast<int32>(chunk.size()), &read);
        if (read < 0 || read > static_cast<int32>(chunk.size()))
            return false;
        if (read != 0) {
            if (bytes.size() + static_cast<std::size_t>(read) >
                    maximum_state_size)
                return false;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);
        }
        if (read == 0)
            return result == kResultOk || result == kResultFalse;
        if (result != kResultOk)
            return false;
    }
}

std::uint8_t midi7(const float value) {
    return static_cast<std::uint8_t>(std::clamp(
            std::lround(static_cast<double>(value) * 127.0), 0L, 127L));
}

class Processor final : public AudioEffect {
  public:
    Processor() {
        // flushes can come before activation, so reserve now. bigger batches
        // grow instead of dropping events
        midi_buffer.reserve(1024);
        parameter_buffer.reserve(1024);
        setControllerClass(controller_id);
        host_parameters.fill(0.5F);
    }

    static FUnknown *create(void *) {
        return static_cast<IAudioProcessor *>(new Processor);
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        const auto result = AudioEffect::initialize(context);
        if (result != kResultOk)
            return result;
        addAudioOutput(STR16("Stereo Output"), SpeakerArr::kStereo);
        addEventInput(STR16("MIDI Input"), 16);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs,
            const int32 input_count, SpeakerArrangement *outputs,
            const int32 output_count) override {
        if (input_count != 0 || output_count != 1 || !outputs ||
                outputs[0] != SpeakerArr::kStereo)
            return kResultFalse;
        return AudioEffect::setBusArrangements(
                inputs, input_count, outputs, output_count);
    }

    tresult PLUGIN_API canProcessSampleSize(
            const int32 symbolic_size) override {
        return symbolic_size == kSample32 ? kResultTrue : kResultFalse;
    }

    uint32 PLUGIN_API getProcessContextRequirements() override {
        return IProcessContextRequirements::kNeedTempo;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        const auto result = AudioEffect::setupProcessing(setup);
        if (result != kResultOk)
            return result;
        sample_rate = setup.sampleRate;
        maximum_block_size = static_cast<std::uint32_t>(
                std::max<int32>(setup.maxSamplesPerBlock, 1));
        return kResultOk;
    }

    tresult PLUGIN_API setActive(const TBool active) override {
        if (active) {
            engine.activate(sample_rate, maximum_block_size);
            is_active = true;
        } else {
            is_active = false;
            engine.deactivate();
        }
        return AudioEffect::setActive(active);
    }

    tresult PLUGIN_API process(ProcessData &data) override {
        if (data.numSamples < 0)
            return kInvalidArgument;
        if (data.processContext &&
                (data.processContext->state & ProcessContext::kTempoValid) != 0)
            engine.set_tempo(data.processContext->tempo);

        auto &midi_events = midi_buffer;
        auto &parameter_events = parameter_buffer;
        midi_events.clear();
        parameter_events.clear();
        if (data.inputEvents) {
            const auto count = data.inputEvents->getEventCount();
            for (int32 index = 0; index < count; ++index) {
                Event event{};
                if (data.inputEvents->getEvent(index, event) != kResultOk ||
                        event.sampleOffset < 0 ||
                        event.sampleOffset >= data.numSamples)
                    continue;
                const auto frame =
                        static_cast<std::uint32_t>(event.sampleOffset);
                switch (event.type) {
                    case Event::kNoteOnEvent:
                        if (event.noteOn.pitch >= 0 && event.noteOn.pitch < 128)
                            midi_events.push_back({ PluginMidiKind::note_on,
                                    frame,
                                    static_cast<std::uint8_t>(std::clamp<int>(
                                            event.noteOn.channel, 0, 15)),
                                    static_cast<std::uint8_t>(
                                            event.noteOn.pitch),
                                    midi7(event.noteOn.velocity), 0 });
                        break;
                    case Event::kNoteOffEvent:
                        if (event.noteOff.pitch >= 0 &&
                                event.noteOff.pitch < 128)
                            midi_events.push_back({ PluginMidiKind::note_off,
                                    frame,
                                    static_cast<std::uint8_t>(std::clamp<int>(
                                            event.noteOff.channel, 0, 15)),
                                    static_cast<std::uint8_t>(
                                            event.noteOff.pitch),
                                    midi7(event.noteOff.velocity), 0 });
                        break;
                    case Event::kPolyPressureEvent:
                        if (event.polyPressure.pitch >= 0 &&
                                event.polyPressure.pitch < 128)
                            midi_events.push_back(
                                    { PluginMidiKind::poly_pressure, frame,
                                            static_cast<std::uint8_t>(
                                                    std::clamp<int>(
                                                            event.polyPressure
                                                                    .channel,
                                                            0, 15)),
                                            static_cast<std::uint8_t>(
                                                    event.polyPressure.pitch),
                                            midi7(event.polyPressure.pressure),
                                            0 });
                        break;
                    default:
                        break;
                }
            }
        }

        if (data.inputParameterChanges) {
            const auto queue_count =
                    data.inputParameterChanges->getParameterCount();
            for (int32 queue_index = 0; queue_index < queue_count;
                    ++queue_index) {
                auto *queue = data.inputParameterChanges->getParameterData(
                        queue_index);
                if (!queue)
                    continue;
                const auto id =
                        static_cast<std::uint32_t>(queue->getParameterId());
                if (id >= instrument_parameter_count)
                    continue;
                const auto point_count = queue->getPointCount();
                for (int32 point_index = 0; point_index < point_count;
                        ++point_index) {
                    int32 offset{};
                    ParamValue value{};
                    if (queue->getPoint(point_index, offset, value) !=
                                    kResultOk ||
                            offset < 0 ||
                            (data.numSamples > 0 && offset >= data.numSamples))
                        continue;
                    const auto bounded = std::clamp(value, 0.0, 1.0);
                    host_parameters[id] = static_cast<float>(bounded);
                    parameter_events.push_back(
                            { static_cast<std::uint32_t>(offset), id, bounded,
                                    true });
                }
            }
        }

        // a parameter flush can have frames but no channels
        if (data.numOutputs == 0 || !data.outputs || data.numSamples == 0 ||
                data.outputs[0].numChannels == 0) {
            engine.process_realtime({}, {}, {}, parameter_events);
            return kResultOk;
        }
        if (!is_active || data.symbolicSampleSize != kSample32 ||
                data.outputs[0].numChannels < 2 ||
                !data.outputs[0].channelBuffers32 ||
                !data.outputs[0].channelBuffers32[0] ||
                !data.outputs[0].channelBuffers32[1])
            return kResultFalse;
        const auto frames = static_cast<std::size_t>(data.numSamples);
        if (data.processMode == ProcessModes::kOffline) {
            if (!engine.process({ data.outputs[0].channelBuffers32[0], frames },
                        { data.outputs[0].channelBuffers32[1], frames },
                        midi_events, parameter_events))
                return kResultFalse;
        } else
            engine.process_realtime(
                    { data.outputs[0].channelBuffers32[0], frames },
                    { data.outputs[0].channelBuffers32[1], frames },
                    midi_events, parameter_events);
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream *state) override {
        const auto bytes = engine.save_state();
        return write_all(state, bytes) ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setState(IBStream *state) override {
        std::vector<std::byte> bytes;
        if (!read_all(state, bytes) || !engine.load_state(bytes))
            return kResultFalse;
        for (const auto &parameter : engine.parameters()) {
            if (parameter.id < host_parameters.size())
                host_parameters[parameter.id] = static_cast<float>(
                        normalized(parameter, parameter.value));
        }
        publish_parameters();
        return kResultOk;
    }

    tresult PLUGIN_API connect(IConnectionPoint *other) override {
        const auto result = AudioEffect::connect(other);
        if (result == kResultOk)
            publish_parameters();
        return result;
    }

    tresult PLUGIN_API notify(IMessage *message) override {
        if (!message || !message->getMessageID() ||
                std::strcmp(message->getMessageID(), editor_state_message_id) !=
                        0)
            return AudioEffect::notify(message);
        const void *data{};
        uint32 size{};
        auto *attributes = message->getAttributes();
        if (!attributes ||
                attributes->getBinary(editor_state_data_id, data, size) !=
                        kResultOk ||
                !data || size > maximum_state_size)
            return kInvalidArgument;
        const auto bytes = std::span{ static_cast<const std::byte *>(data),
            static_cast<std::size_t>(size) };
        const bool loaded = engine.load_state(bytes);
        if (trace_state_exchange())
            std::fprintf(stderr,
                    "ekphore-vst3 processor state bytes=%u loaded=%d\n", size,
                    loaded ? 1 : 0);
        if (!loaded)
            return kResultFalse;
        for (const auto &parameter : engine.parameters())
            if (parameter.id < host_parameters.size())
                host_parameters[parameter.id] = static_cast<float>(
                        normalized(parameter, parameter.value));
        publish_parameters();
        return kResultOk;
    }

  private:
    void publish_parameters() {
        auto message = owned(allocateMessage());
        if (!message)
            return;
        message->setMessageID(parameter_message_id);
        auto *attributes = message->getAttributes();
        if (!attributes)
            return;
        const auto bytes = encode_parameters(engine.parameters());
        if (bytes.size() > std::numeric_limits<uint32>::max())
            return;
        if (attributes->setBinary(parameter_data_id, bytes.data(),
                    static_cast<uint32>(bytes.size())) == kResultOk)
            (void)sendMessage(message);
    }

    Plugin engine;
    // process/flush only, never at the same time
    std::vector<PluginMidiEvent> midi_buffer;
    std::vector<PluginParameterEvent> parameter_buffer;
    std::array<float, instrument_parameter_count> host_parameters{};
    double sample_rate{ 44100.0 };
    std::uint32_t maximum_block_size{ 512 };
    bool is_active{};
};

class Controller final : public EditController {
  public:
    Controller() { rebuild_parameters({}); }

    static FUnknown *create(void *) {
        return static_cast<IEditController *>(new Controller);
    }

    tresult PLUGIN_API setComponentState(IBStream *state) override {
        if (!state)
            return kInvalidArgument;
        std::vector<std::byte> bytes;
        if (!read_all(state, bytes) || !editor_engine.load_state(bytes))
            return kResultFalse;
        if (view)
            invalidate_view();
        return kResultOk;
    }

    IPlugView *PLUGIN_API createView(FIDString name) override;

    tresult PLUGIN_API notify(IMessage *message) override {
        if (!message || !message->getMessageID() ||
                std::strcmp(message->getMessageID(), parameter_message_id) != 0)
            return EditController::notify(message);
        const void *data{};
        uint32 size{};
        auto *attributes = message->getAttributes();
        if (!attributes ||
                attributes->getBinary(parameter_data_id, data, size) !=
                        kResultOk ||
                !data || size > 4U * 1024U * 1024U)
            return kInvalidArgument;
        std::vector<PluginParameter> snapshot;
        if (!decode_parameters(
                    { static_cast<const std::byte *>(data), size }, snapshot))
            return kInvalidArgument;

        rebuild_parameters(snapshot);
        if (componentHandler)
            componentHandler->restartComponent(
                    RestartFlags::kParamTitlesChanged |
                    RestartFlags::kParamValuesChanged);
        return kResultOk;
    }

  private:
    class View;
    void invalidate_view();
    void publish_editor_state();

    void rebuild_parameters(const std::span<const PluginParameter> snapshot) {
        parameters.removeAll();
        for (std::uint32_t id = 0; id < instrument_parameter_count; ++id) {
            const auto assigned =
                    std::ranges::find(snapshot, id, &PluginParameter::id);
            if (assigned == snapshot.end()) {
                String128 title{};
                const auto name = inactive_parameter_name(id);
                if (!StringConvert::convert(name, title))
                    continue;
                parameters.addParameter(new Parameter(title, id, nullptr, 0.5,
                        kStepCountContinuous, ParameterInfo::kCanAutomate));
                continue;
            }
            const auto &parameter = *assigned;
            String128 title{};
            if (!StringConvert::convert(parameter.name, title))
                continue;
            const auto extent = parameter.maximum - parameter.minimum;
            const auto steps =
                    !parameter.element && extent >= 0.0 &&
                                    extent <= static_cast<double>(
                                                      std::numeric_limits<
                                                              int32>::max())
                            ? static_cast<int32>(std::llround(extent))
                            : kStepCountContinuous;
            const auto flags = parameter.automatable
                                       ? ParameterInfo::kCanAutomate
                                       : ParameterInfo::kNoFlags;
            auto *published = new RangeParameter(title, parameter.id, nullptr,
                    parameter.minimum, parameter.maximum,
                    parameter.default_value, steps, flags);
            published->setNormalized(normalized(parameter, parameter.value));
            parameters.addParameter(published);
        }
    }

    Plugin editor_engine;
    View *view{};
};

class Controller::View final : public CPluginView,
                               public IPlugViewContentScaleSupport {
  public:
    View(Controller &controller, Plugin &engine)
        : controller(controller),
          session(engine, [&controller] { controller.publish_editor_state(); }),
          editor(session) {
        const auto [width, height] = editor.size();
        rect = { 0, 0, static_cast<Steinberg::int32>(width),
            static_cast<Steinberg::int32>(height) };
        controller.view = this;
    }

    ~View() override {
        if (controller.view == this)
            controller.view = nullptr;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void **object) override {
        QUERY_INTERFACE(iid, object, IPlugViewContentScaleSupport::iid,
                IPlugViewContentScaleSupport)
        return CPluginView::queryInterface(iid, object);
    }

    uint32 PLUGIN_API addRef() override { return CPluginView::addRef(); }
    uint32 PLUGIN_API release() override { return CPluginView::release(); }

    tresult PLUGIN_API setContentScaleFactor(
            const ScaleFactor factor) override {
        if (!std::isfinite(factor) || factor <= 0.0F)
            return kInvalidArgument;
        if (!editor.set_scale(factor))
            return kResultFalse;
        const auto [width, height] = editor.size();
        rect = { 0, 0, static_cast<Steinberg::int32>(width),
            static_cast<Steinberg::int32>(height) };
        if (plugFrame)
            return plugFrame->resizeView(this, &rect);
        return kResultTrue;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        const auto api = platform(type);
        return api && NativeEditor::supports(*api) ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API attached(void *parent, FIDString type) override {
        if (isAttached() || !parent)
            return kInvalidArgument;
        const auto api = platform(type);
        if (!api || !editor.attach({ *api, parent, nullptr }))
            return kResultFalse;
        systemWindow = parent;
        editor.show();
        return kResultTrue;
    }

    tresult PLUGIN_API removed() override {
        editor.detach();
        systemWindow = nullptr;
        return kResultTrue;
    }

    tresult PLUGIN_API onSize(ViewRect *size) override {
        return size && size->left == rect.left && size->top == rect.top &&
                               size->right == rect.right &&
                               size->bottom == rect.bottom
                       ? kResultTrue
                       : kResultFalse;
    }

    void invalidate() {
        session.invalidate();
        editor.idle();
    }

  private:
    static std::optional<NativeWindowApi> platform(FIDString type) {
        if (!type)
            return std::nullopt;
        if (!std::strcmp(type, kPlatformTypeNSView))
            return NativeWindowApi::cocoa;
        if (!std::strcmp(type, kPlatformTypeHWND))
            return NativeWindowApi::win32;
        if (!std::strcmp(type, kPlatformTypeX11EmbedWindowID))
            return NativeWindowApi::x11;
        return std::nullopt;
    }

    Controller &controller;
    EditorSession session;
    NativeEditor editor;
};

void Controller::invalidate_view() { view->invalidate(); }

void Controller::publish_editor_state() {
    const auto bytes = editor_engine.save_state();
    if (bytes.empty() || bytes.size() > maximum_state_size ||
            bytes.size() > std::numeric_limits<uint32>::max())
        return;
    auto message = owned(allocateMessage());
    if (!message) {
        if (trace_state_exchange())
            std::fprintf(stderr,
                    "ekphore-vst3 controller state bytes=%zu no-message\n",
                    bytes.size());
        return;
    }
    message->setMessageID(editor_state_message_id);
    auto *attributes = message->getAttributes();
    if (!attributes)
        return;
    const auto assigned = attributes->setBinary(editor_state_data_id,
            bytes.data(), static_cast<uint32>(bytes.size()));
    const auto sent = assigned == kResultOk ? sendMessage(message) : assigned;
    if (trace_state_exchange())
        std::fprintf(stderr,
                "ekphore-vst3 controller state bytes=%zu assigned=%d sent=%d\n",
                bytes.size(), static_cast<int>(assigned),
                static_cast<int>(sent));
}

IPlugView *Controller::createView(FIDString name) {
    if (!name || std::strcmp(name, ViewType::kEditor) != 0)
        return nullptr;
    return new View(*this, editor_engine);
}

} // namespace ekphore::vst3

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF("GovanifY", "https://ekphore.github.io", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(ekphore::vst3::processor_id),
        PClassInfo::kManyInstances, kVstAudioEffectClass, "ekphore",
        Vst::kDistributable, Vst::PlugType::kInstrumentSampler, "0.1.0",
        kVstVersionString, ekphore::vst3::Processor::create)

DEF_CLASS2(INLINE_UID_FROM_FUID(ekphore::vst3::controller_id),
        PClassInfo::kManyInstances, kVstComponentControllerClass,
        "ekphore Controller", 0, "", "0.1.0", kVstVersionString,
        ekphore::vst3::Controller::create)

END_FACTORY
