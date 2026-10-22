// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AudioToolbox/AudioToolbox.h>

namespace ekphore::au {

inline constexpr AudioUnitPropertyID engine_property =
        static_cast<AudioUnitPropertyID>(
                'E' << 24 | 'k' << 16 | 'U' << 8 | 'I');
inline constexpr const char *bundle_identifier = "io.github.ekphore.au";
inline constexpr const char *factory_class = "EkphoreCocoaViewFactory";

} // namespace ekphore::au
