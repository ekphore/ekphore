// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/plugin.h>

#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/MusicDeviceBase.h>

#include "../realtime/queues/bounded_mpsc_queue.h"
#include "au_ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

const CFStringRef state_key = CFSTR("io.github.ekphore.state");

ekphore::PluginMidiEvent midi_event(ekphore::PluginMidiKind kind, UInt8 channel,
        UInt8 data_1, UInt8 data_2, UInt32 frame) {
    return { kind, frame, channel, data_1, data_2, 0 };
}

} // namespace

class EkphoreAudioUnit final : public ausdk::MusicDeviceBase {
  public:
    explicit EkphoreAudioUnit(AudioComponentInstance instance)
        : MusicDeviceBase(instance, 0, 1),
          engine(std::make_shared<ekphore::nks::Client>(
                  ekphore::nks::runtime_directory(),
                  libnks::OdrInstanceType::audio_unit)) {}

    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope,
            AudioUnitElement element, UInt32 &size, bool &writable) override {
        if (scope == kAudioUnitScope_Global && element == 0) {
            switch (id) {
                case kAudioUnitProperty_OfflineRender:
                    size = sizeof(UInt32);
                    writable = true;
                    return noErr;
                case kAudioUnitProperty_CocoaUI:
                    size = sizeof(AudioUnitCocoaViewInfo);
                    writable = false;
                    return noErr;
                case ekphore::au::engine_property:
                    size = sizeof(ekphore::Plugin *);
                    writable = false;
                    return noErr;
                default:
                    break;
            }
        }
        return MusicDeviceBase::GetPropertyInfo(
                id, scope, element, size, writable);
    }

    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope,
            AudioUnitElement element, void *output) override {
        if (!output)
            return kAudioUnitErr_InvalidPropertyValue;
        if (scope == kAudioUnitScope_Global && element == 0 &&
                id == kAudioUnitProperty_OfflineRender) {
            *static_cast<UInt32 *>(output) =
                    offline.load(std::memory_order_relaxed);
            return noErr;
        }
        if (scope == kAudioUnitScope_Global && element == 0 &&
                id == kAudioUnitProperty_CocoaUI) {
            auto &info = *static_cast<AudioUnitCocoaViewInfo *>(output);
            auto *bundle = CFBundleGetBundleWithIdentifier(
                    CFSTR("io.github.ekphore.au"));
            if (!bundle)
                return kAudioUnitErr_InvalidProperty;
            info.mCocoaAUViewBundleLocation = CFBundleCopyBundleURL(bundle);
            info.mCocoaAUViewClass[0] = CFStringCreateWithCString(
                    nullptr, ekphore::au::factory_class, kCFStringEncodingUTF8);
            if (!info.mCocoaAUViewBundleLocation || !info.mCocoaAUViewClass[0])
                return kAudioUnitErr_InvalidProperty;
            return noErr;
        }
        if (scope == kAudioUnitScope_Global && element == 0 &&
                id == ekphore::au::engine_property) {
            *static_cast<ekphore::Plugin **>(output) = &engine;
            return noErr;
        }
        return MusicDeviceBase::GetProperty(id, scope, element, output);
    }

    OSStatus SetProperty(AudioUnitPropertyID id, AudioUnitScope scope,
            AudioUnitElement element, const void *data, UInt32 size) override {
        if (id == kAudioUnitProperty_OfflineRender) {
            if (scope != kAudioUnitScope_Global || element != 0)
                return kAudioUnitErr_InvalidScope;
            if (!data || size != sizeof(UInt32))
                return kAudioUnitErr_InvalidPropertyValue;
            offline.store(*static_cast<const UInt32 *>(data) != 0,
                    std::memory_order_relaxed);
            return noErr;
        }
        return MusicDeviceBase::SetProperty(id, scope, element, data, size);
    }

    bool StreamFormatWritable(
            AudioUnitScope scope, AudioUnitElement element) override {
        return scope == kAudioUnitScope_Output && element == 0 &&
               !IsInitialized();
    }

    UInt32 SupportedNumChannels(const AUChannelInfo **info) override {
        static constexpr AUChannelInfo channels[]{ { 0, 2 } };
        if (info)
            *info = channels;
        return 1;
    }

    bool ValidFormat(AudioUnitScope scope, AudioUnitElement element,
            const AudioStreamBasicDescription &format) override {
        return scope == kAudioUnitScope_Output && element == 0 &&
               format.mChannelsPerFrame == 2 &&
               MusicDeviceBase::ValidFormat(scope, element, format);
    }

    bool CanScheduleParameters() const noexcept override { return false; }

    OSStatus GetParameterList(AudioUnitScope scope,
            AudioUnitParameterID *output, UInt32 &count) override {
        if (scope != kAudioUnitScope_Global) {
            count = 0;
            return noErr;
        }
        const auto parameters = published_parameters();
        count = static_cast<UInt32>(parameters.size());
        if (output)
            std::ranges::transform(
                    parameters, output, &ekphore::PluginParameter::id);
        return noErr;
    }

    OSStatus GetParameterInfo(AudioUnitScope scope, AudioUnitParameterID id,
            AudioUnitParameterInfo &info) override {
        if (scope != kAudioUnitScope_Global)
            return kAudioUnitErr_InvalidParameter;
        const auto parameters = published_parameters();
        const auto found = std::ranges::find(parameters,
                static_cast<std::uint32_t>(id), &ekphore::PluginParameter::id);
        if (found == parameters.end())
            return kAudioUnitErr_InvalidParameter;
        info = {};
        std::strncpy(info.name, found->name.c_str(), sizeof(info.name) - 1);
        info.cfNameString = CFStringCreateWithCString(
                nullptr, found->name.c_str(), kCFStringEncodingUTF8);
        if (!info.cfNameString)
            return kAudioUnitErr_InvalidParameterValue;
        info.unit = found->element ? kAudioUnitParameterUnit_Generic
                                   : kAudioUnitParameterUnit_Indexed;
        info.minValue = static_cast<AudioUnitParameterValue>(found->minimum);
        info.maxValue = static_cast<AudioUnitParameterValue>(found->maximum);
        info.defaultValue =
                static_cast<AudioUnitParameterValue>(found->default_value);
        info.flags = kAudioUnitParameterFlag_IsReadable |
                     kAudioUnitParameterFlag_IsWritable |
                     kAudioUnitParameterFlag_HasCFNameString |
                     kAudioUnitParameterFlag_CFNameRelease;
        if (found->element)
            info.flags |= kAudioUnitParameterFlag_IsHighResolution;
        return noErr;
    }

    OSStatus GetParameter(AudioUnitParameterID id, AudioUnitScope scope,
            AudioUnitElement element, AudioUnitParameterValue &value) override {
        if (scope != kAudioUnitScope_Global || element != 0)
            return kAudioUnitErr_InvalidParameter;
        const auto parameters = published_parameters();
        const auto found = std::ranges::find(parameters,
                static_cast<std::uint32_t>(id), &ekphore::PluginParameter::id);
        if (found == parameters.end())
            return kAudioUnitErr_InvalidParameter;
        value = static_cast<AudioUnitParameterValue>(found->value);
        return noErr;
    }

    OSStatus SetParameter(AudioUnitParameterID id, AudioUnitScope scope,
            AudioUnitElement element, AudioUnitParameterValue value,
            UInt32) override {
        if (scope != kAudioUnitScope_Global || element != 0)
            return kAudioUnitErr_InvalidParameter;
        if (!engine.set_parameter(static_cast<std::uint32_t>(id), value))
            return kAudioUnitErr_InvalidParameter;
        return noErr;
    }

    OSStatus Initialize() override {
        const auto result = MusicDeviceBase::Initialize();
        if (result != noErr)
            return result;
        const auto &format = Output(0).GetStreamFormat();
        engine.activate(format.mSampleRate, GetMaxFramesPerSlice());
        return noErr;
    }

    void Cleanup() override {
        engine.deactivate();
        MusicDeviceBase::Cleanup();
    }

    OSStatus Reset(AudioUnitScope scope, AudioUnitElement element) override {
        engine.reset();
        // reset and render must NOT overlap. late producers still have the old
        // generation, the next drain just drops their events
        midi_rejected.store(false, std::memory_order_release);
        midi_generation.fetch_add(1, std::memory_order_acq_rel);
        return MusicDeviceBase::Reset(scope, element);
    }

    OSStatus Render(AudioUnitRenderActionFlags &flags,
            const AudioTimeStamp &timestamp, UInt32 frame_count) override {
        (void)flags;
        (void)timestamp;
        auto &buffers = Output(0).GetBufferList();
        if (buffers.mNumberBuffers != 2 || !buffers.mBuffers[0].mData ||
                !buffers.mBuffers[1].mData) {
            return kAudioUnitErr_FormatNotSupported;
        }

        Float64 beat{};
        Float64 tempo{};
        if (CallHostBeatAndTempo(&beat, &tempo) == noErr && tempo > 0.0)
            engine.set_tempo(tempo);

        const auto events = drain_midi(frame_count);

        auto *left = static_cast<float *>(buffers.mBuffers[0].mData);
        auto *right = static_cast<float *>(buffers.mBuffers[1].mData);
        return process_audio(
                { left, frame_count }, { right, frame_count }, events);
    }

    OSStatus SaveState(CFPropertyListRef *output) override {
        CFPropertyListRef base{};
        const auto result = MusicDeviceBase::SaveState(&base);
        if (result != noErr)
            return result;
        auto *dictionary = CFDictionaryCreateMutableCopy(
                nullptr, 0, static_cast<CFDictionaryRef>(base));
        CFRelease(base);
        if (!dictionary)
            return kAudioUnitErr_FailedInitialization;

        const auto bytes = engine.save_state();
        auto *data = CFDataCreate(nullptr,
                reinterpret_cast<const UInt8 *>(bytes.data()),
                static_cast<CFIndex>(bytes.size()));
        if (!data) {
            CFRelease(dictionary);
            return kAudioUnitErr_FailedInitialization;
        }
        CFDictionarySetValue(dictionary, state_key, data);
        CFRelease(data);
        *output = dictionary;
        return noErr;
    }

    OSStatus RestoreState(CFPropertyListRef state) override {
        const auto result = MusicDeviceBase::RestoreState(state);
        if (result != noErr)
            return result;
        if (CFGetTypeID(state) != CFDictionaryGetTypeID())
            return kAudioUnitErr_InvalidPropertyValue;
        const auto *value = CFDictionaryGetValue(
                static_cast<CFDictionaryRef>(state), state_key);
        if (!value)
            return noErr;
        if (CFGetTypeID(value) != CFDataGetTypeID())
            return kAudioUnitErr_InvalidPropertyValue;
        const auto *data = static_cast<CFDataRef>(value);
        const auto *begin =
                reinterpret_cast<const std::byte *>(CFDataGetBytePtr(data));
        const auto size = static_cast<std::size_t>(CFDataGetLength(data));
        if (!engine.load_state({ begin, size }))
            return kAudioUnitErr_InvalidPropertyValue;
        PropertyChanged(
                kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0);
        return noErr;
    }

  protected:
    OSStatus HandleNoteOn(
            UInt8 channel, UInt8 note, UInt8 velocity, UInt32 frame) override {
        return enqueue(midi_event(ekphore::PluginMidiKind::note_on, channel,
                note, velocity, frame));
    }

    OSStatus HandleNoteOff(
            UInt8 channel, UInt8 note, UInt8 velocity, UInt32 frame) override {
        return enqueue(midi_event(ekphore::PluginMidiKind::note_off, channel,
                note, velocity, frame));
    }

    OSStatus HandleControlChange(UInt8 channel, UInt8 controller, UInt8 value,
            UInt32 frame) override {
        return enqueue(midi_event(ekphore::PluginMidiKind::control_change,
                channel, controller, value, frame));
    }

    OSStatus HandlePitchWheel(
            UInt8 channel, UInt8 low, UInt8 high, UInt32 frame) override {
        auto event = midi_event(
                ekphore::PluginMidiKind::pitch_bend, channel, low, high, frame);
        event.value = (static_cast<std::int32_t>(high) << 7) | low;
        return enqueue(event);
    }

    OSStatus HandleChannelPressure(
            UInt8 channel, UInt8 value, UInt32 frame) override {
        return enqueue(midi_event(ekphore::PluginMidiKind::channel_pressure,
                channel, value, 0, frame));
    }

    OSStatus HandlePolyPressure(
            UInt8 channel, UInt8 note, UInt8 value, UInt32 frame) override {
        return enqueue(midi_event(ekphore::PluginMidiKind::poly_pressure,
                channel, note, value, frame));
    }

  private:
    OSStatus process_audio(std::span<float> left, std::span<float> right,
            std::span<const ekphore::PluginMidiEvent> events) {
        if (offline.load(std::memory_order_relaxed)) {
            const bool complete = engine.process(left, right, events);
            // the host can ignore MIDI errors but the export still has to
            // fail, so keep it latched until reset
            if (!complete || midi_rejected.load(std::memory_order_acquire))
                return kAudioUnitErr_InvalidOfflineRender;
        } else
            engine.process_realtime(left, right, events);
        return noErr;
    }

    std::vector<ekphore::PluginParameter> published_parameters() const {
        auto parameters = engine.parameters();
        std::erase_if(parameters,
                [](const auto &parameter) { return !parameter.automatable; });
        return parameters;
    }

    OSStatus enqueue(ekphore::PluginMidiEvent event) noexcept {
        if (pending_midi.try_push(
                    { event, midi_generation.load(std::memory_order_acquire) }))
            return noErr;
        midi_rejected.store(true, std::memory_order_release);
        return kAudioUnitErr_CannotDoInCurrentContext;
    }

    std::span<const ekphore::PluginMidiEvent> drain_midi(
            UInt32 frames) noexcept {
        const auto generation = midi_generation.load(std::memory_order_acquire);
        std::size_t count = 0;
        QueuedMidi input;
        // producers can refill while we drain, cap it or we never stop
        for (std::size_t read = 0;
                read < midi_capacity && pending_midi.pop(input); ++read) {
            if (input.generation != generation)
                continue;
            input.event.frame = std::min(input.event.frame, frames);
            ordered_midi[count] = { input.event, count };
            ++count;
        }
        // the ordinal keeps the order, stable_sort would allocate
        std::sort(ordered_midi.begin(), ordered_midi.begin() + count,
                [](const auto &a, const auto &b) {
                    return a.event.frame != b.event.frame
                                   ? a.event.frame < b.event.frame
                                   : a.ordinal < b.ordinal;
                });
        for (std::size_t i = 0; i < count; ++i)
            midi_batch[i] = ordered_midi[i].event;
        return { midi_batch.data(), count };
    }

    ekphore::Plugin engine;
    std::atomic<bool> offline{};
    static constexpr std::size_t midi_capacity = 1024;
    struct QueuedMidi {
        ekphore::PluginMidiEvent event{};
        std::size_t generation{};
    };
    struct OrderedMidi {
        ekphore::PluginMidiEvent event{};
        std::size_t ordinal{};
    };
    std::atomic<std::size_t> midi_generation{};
    std::atomic<bool> midi_rejected{};
    ekphore::dsp::BoundedMpscQueue<QueuedMidi, midi_capacity> pending_midi;
    std::array<OrderedMidi, midi_capacity> ordered_midi{};
    std::array<ekphore::PluginMidiEvent, midi_capacity> midi_batch{};
};

AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, EkphoreAudioUnit)
