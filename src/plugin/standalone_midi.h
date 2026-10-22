// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ekphore/plugin.h>
#include <iostream>
#include <libnks/midi.h>
#include <map>

namespace ekphore::standalone {
// packets can split messages and realtime bytes can land in the middle, so one
// of these per source
class MidiDecoder {
    std::uint8_t status{}, count{}, needed{};
    std::array<std::uint8_t, 2> data{};
    bool sysex{};

  public:
    template <class Emit>
    void receive(std::span<const std::uint8_t> bytes, Emit emit) {
        for (auto byte : bytes) {
            if (byte >= 0xf8)
                continue;
            if (byte & 0x80) {
                count = 0;
                if (byte >= 0xf0) {
                    status = 0;
                    sysex = byte == 0xf0;
                    continue;
                }
                sysex = false;
                status = byte;
                needed = (byte & 0xe0) == 0xc0 ? 1 : 2;
                continue;
            }
            if (sysex || !status)
                continue;
            data[count++] = byte;
            if (count != needed)
                continue;
            count = 0;
            PluginMidiEvent event{};
            event.channel = status & 15;
            event.data_1 = data[0];
            event.data_2 = needed == 2 ? data[1] : 0;
            switch (status & 0xf0) {
                case 0x80:
                    event.kind = PluginMidiKind::note_off;
                    break;
                case 0x90:
                    event.kind = event.data_2 ? PluginMidiKind::note_on
                                              : PluginMidiKind::note_off;
                    break;
                case 0xa0:
                    event.kind = PluginMidiKind::poly_pressure;
                    break;
                case 0xb0:
                    event.kind = PluginMidiKind::control_change;
                    break;
                case 0xd0:
                    event.kind = PluginMidiKind::channel_pressure;
                    event.value = data[0];
                    break;
                case 0xe0:
                    event.kind = PluginMidiKind::pitch_bend;
                    event.value = (int(data[1]) << 7) + data[0] - 8192;
                    break;
                default:
                    continue; // program changes don't select instruments here
            }
            emit(event);
        }
    }
};

class MidiInputs {
    struct Source {
        libnks::MidiDevice device;
        MidiDecoder decoder;
    };
    std::map<std::string, Source> sources;
    std::chrono::steady_clock::time_point next_scan{};

  public:
    template <class Emit>
    void poll(Emit emit) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_scan) {
            next_scan = now + std::chrono::seconds(2);
            const auto ports = libnks::MidiDevice::enumerate();
            const bool trace = std::getenv("EKPHORE_MIDI_TRACE") != nullptr;
            if (trace)
                std::cerr << "MIDI scan: " << ports.size()
                          << " available ports\n";
            for (auto it = sources.begin(); it != sources.end();) {
                if (std::none_of(ports.begin(), ports.end(),
                            [&](const auto &p) { return p.id == it->first; })) {
                    // unplug won't send note-offs; release sustain and voices
                    for (std::uint8_t channel = 0; channel < 16; ++channel)
                        for (const auto cc : { 64, 123, 120 })
                            emit(PluginMidiEvent{
                                    PluginMidiKind::control_change, 0, channel,
                                    std::uint8_t(cc), 0 });
                    it = sources.erase(it);
                } else
                    ++it;
            }
            const auto *selected = std::getenv("EKPHORE_MIDI_INPUT");
            for (const auto &port : ports) {
                if (trace)
                    std::cerr << "MIDI port: " << port.name << " id=" << port.id
                              << '\n';
                if (selected && std::string_view(selected) == "none")
                    break;
                if (selected && *selected && port.id != selected &&
                        port.name != selected)
                    continue;
                // DAW ports are surface CCs, don't pick them automatically
                std::string lower = port.name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return char(std::tolower(c)); });
                if ((!selected || !*selected) &&
                        lower.find("daw") != std::string::npos)
                    continue;
                if (sources.contains(port.id))
                    continue;
                Source source;
                if (!source.device.open_input(port.id)) {
                    if (trace)
                        std::cerr << "MIDI open failed: " << port.name
                                  << " error=" << source.device.last_error()
                                  << '\n';
                    continue;
                }
                std::cerr << "MIDI input connected: " << port.name << '\n';
                sources.emplace(port.id, std::move(source));
            }
            if (trace)
                std::cerr << "MIDI scan: " << sources.size()
                          << " connected inputs\n";
        }
        std::vector<std::uint8_t> bytes;
        for (auto &[id, source] : sources)
            for (int i = 0; i < 256 && source.device.receive(bytes,
                                               std::chrono::milliseconds(0));
                    ++i)
                source.decoder.receive(
                        bytes, [&](const PluginMidiEvent &event) {
                            if (std::getenv("EKPHORE_MIDI_TRACE"))
                                std::cerr
                                        << "MIDI received: " << id
                                        << " kind=" << int(event.kind)
                                        << " channel=" << int(event.channel) + 1
                                        << " data=" << int(event.data_1) << ','
                                        << int(event.data_2) << '\n';
                            emit(event);
                        });
    }
};
} // namespace ekphore::standalone
