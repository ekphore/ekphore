// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "observations.h"

namespace ekphore::detail {

std::string ExternalObservations::key(
        std::initializer_list<std::string_view> fields) {
    std::string result;
    for (const auto field : fields) {
        result += std::to_string(field.size());
        result += ':';
        result += field;
    }
    return result;
}

void ExternalObservations::begin_record() {
    entries.clear();
    cursor = 0;
    current_mode = Mode::record;
}

void ExternalObservations::begin_replay() {
    cursor = 0;
    current_mode = Mode::replay;
}

void ExternalObservations::finish_replay() const {
    if (current_mode != Mode::replay || cursor != entries.size())
        throw Divergence("incomplete external observation replay");
}

void ExternalObservations::clear() {
    entries.clear();
    cursor = 0;
    current_mode = Mode::live;
}

sampling::wavetable::OscillatorResetState observe_source_reset(
        ExternalObservations &external, SourceResetIdentity identity,
        float seed, float randomization, float fm_seed) {
    return external.observe<sampling::wavetable::OscillatorResetState>(
            ExternalObservations::key({ "wavetable-source-reset",
                    std::to_string(identity.voice),
                    std::to_string(identity.event),
                    std::to_string(identity.group),
                    std::to_string(identity.sample),
                    std::to_string(std::bit_cast<std::uint32_t>(seed)),
                    std::to_string(std::bit_cast<std::uint32_t>(randomization)),
                    std::to_string(std::bit_cast<std::uint32_t>(fm_seed)) }),
            [&] {
                sampling::wavetable::OscillatorResetState state;
                sampling::wavetable::reset_oscillator(state, seed,
                        randomization, fm_seed, [] { return std::rand(); });
                return state;
            });
}

} // namespace ekphore::detail
