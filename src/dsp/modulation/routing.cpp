// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "routing.h"
#include <libnki/model.h>
#include <string_view>

namespace ekphore::dsp {

ExternalTargetClassification classify_external_target(
        const libnki::ModulationTargetInfo &target) {
    ExternalTargetClassification result;
    // the ID and name can disagree and either one counts, so don't stop at the
    // first match
    result.volume = target.id == 2 || target.name == "volume";
    result.pan = target.id == 3 || target.name == "pan";
    result.pitch = target.id == 6 || target.name == "pitch";
    result.cutoff = target.id == 0x1f || target.name == "filterCutoff";
    result.dyx_morph = target.id == 0x43 || target.name == "dyx_morph";
    result.dyx_dry = target.id == 0x44 || target.name == "dyx_dry";
    result.position = target.id == 13 || target.name == "wavetablePosition";
    static constexpr std::array<std::string_view, 8> sends{ "sendLevel0",
        "sendLevel1", "sendLevel2", "sendLevel3", "sendLevel4", "sendLevel5",
        "sendLevel6", "sendLevel7" };
    static constexpr std::array<std::string_view, 5> legacy_sends{
        "sendLevelDelay", "sendLevelChorus", "sendLevelFlanger",
        "sendLevelPhaser", "sendLevelReverb"
    };
    static constexpr std::array<std::string_view, 9> eq{ "eqFreq1",
        "eqBandWidth1", "eqGain1", "eqFreq2", "eqBandWidth2", "eqGain2",
        "eqFreq3", "eqBandWidth3", "eqGain3" };
    if (target.id && *target.id >= 0x71 && *target.id <= 0x78)
        result.send = *target.id - 0x71;
    else {
        for (std::size_t i = 0; i < sends.size(); ++i)
            if (target.name == sends[i]) {
                result.send = i;
                break;
            }
        if (!result.send)
            for (std::size_t i = 0; i < legacy_sends.size(); ++i)
                if (target.name == legacy_sends[i]) {
                    result.send = i;
                    break;
                }
    }
    if (target.id && *target.id >= 0x28 && *target.id <= 0x30)
        result.eq = *target.id - 0x28;
    else
        for (std::size_t i = 0; i < eq.size(); ++i)
            if (target.name == eq[i]) {
                result.eq = i;
                break;
            }
    return result;
}

} // namespace ekphore::dsp
