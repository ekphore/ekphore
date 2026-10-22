// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "trace.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace ekphore::detail {
// flush every line or a crash eats the last gestures
void gesture_trace(const char *format, ...) {
    const auto *path = std::getenv("EKPHORE_UI_GESTURE_TRACE");
    if (!path || !*path)
        return;
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    static FILE *output = std::fopen(path, "a");
    if (!output)
        return;
    const auto time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::fprintf(output, "%lld ", static_cast<long long>(time));
    va_list args;
    va_start(args, format);
    std::vfprintf(output, format, args);
    va_end(args);
    std::fputc('\n', output);
    std::fflush(output);
}
} // namespace ekphore::detail
