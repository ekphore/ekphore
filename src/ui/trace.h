// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ekphore::detail {
// no I/O unless EKPHORE_UI_GESTURE_TRACE supplies a path
void gesture_trace(const char *format, ...);
} // namespace ekphore::detail
