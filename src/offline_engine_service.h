// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ekphore/player.h"
#include <stdexcept>

namespace ekphore {

// offline only, NOT from audio. this gets the result, the completion callback
// still goes out with event processing
inline void service_offline_engine_commands(Player &player) {
    const auto batch = player.snapshot_engine_commands();
    for (const auto command : batch) {
        if (!player.execute_engine_command(command))
            continue;
        if (!player.dispatch_engine_command_completion(command))
            throw std::runtime_error(
                    "offline engine command completion failed");
    }
    // callbacks can queue more, leave it for next time or we spin forever
}

} // namespace ekphore
