// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <ekphore/plugin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <mutex>

namespace ekphore {

// triple buffer, only readers take the mutex so audio never waits on a host
// copying parameters
struct PluginParameterPublication {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned dirty = 4;
    std::array<std::vector<PluginParameter>, 3> buffers;
    std::array<std::size_t, 3> sizes{};
    std::atomic<unsigned> middle{ 1 };
    unsigned back{ 2 }; // writer only
    unsigned front{};   // readers only
    std::mutex readers;

    // NOT while audio runs, and fill all three BEFORE the first callback
    void prepare(std::span<const UiControl> controls) {
        std::scoped_lock lock(readers);
        for (unsigned i = 0; i < 3; ++i) {
            publish(controls);
            front = middle.exchange(front, std::memory_order_acq_rel) & ~dirty;
        }
    }

    void publish(std::span<const UiControl> controls) {
        auto &output = buffers[back];
        std::size_t count{};
        const auto number = [](const libksp::Value &value, double fallback) {
            if (const auto *integer = std::get_if<std::int64_t>(&value))
                return static_cast<double>(*integer);
            if (const auto *real = std::get_if<double>(&value))
                return *real;
            return fallback;
        };
        // reuse everything, only a new schema allocates
        const auto append = [&](const UiControl &control, std::uint32_t id,
                                    std::optional<std::size_t> element,
                                    std::string_view name, double minimum,
                                    double maximum, double initial,
                                    double value,
                                    bool automatable) -> PluginParameter & {
            if (count == output.size())
                output.emplace_back();
            auto &parameter = output[count++];
            parameter.id = id;
            parameter.script_slot = control.script_slot;
            parameter.control = control.name;
            parameter.element = element;
            parameter.name = name;
            parameter.minimum = minimum;
            parameter.maximum = maximum;
            parameter.default_value = initial;
            parameter.value = value;
            parameter.automatable = automatable;
            return parameter;
        };
        for (const auto &control : controls) {
            const std::string_view name =
                    !control.automation_name.empty() ? control.automation_name
                    : !control.short_name.empty()    ? control.short_name
                                                     : control.name;
            if (control.automation_id >= 0)
                append(control,
                        static_cast<std::uint32_t>(control.automation_id), {},
                        name, control.minimum, control.maximum,
                        number(control.default_value, control.minimum),
                        number(control.value, control.minimum),
                        control.allow_automation);
            if (control.kind != UiControlKind::xy)
                continue;
            const auto *values =
                    std::get_if<std::vector<double>>(&control.value);
            const auto *defaults =
                    std::get_if<std::vector<double>>(&control.default_value);
            for (std::size_t i = 0; i < control.xy_automation_ids.size(); ++i) {
                const auto id = control.xy_automation_ids[i];
                if (id < 0)
                    continue;
                const bool named = i < control.xy_automation_names.size() &&
                                   !control.xy_automation_names[i].empty();
                auto &parameter = append(control,
                        static_cast<std::uint32_t>(id), i,
                        named ? std::string_view(control.xy_automation_names[i])
                              : name,
                        0.0, 1.0,
                        defaults && i < defaults->size() ? (*defaults)[i] : 0.0,
                        values && i < values->size() ? (*values)[i] : 0.0,
                        true);
                if (!named) {
                    char index[32];
                    const auto converted =
                            std::to_chars(index, index + sizeof(index), i + 1);
                    parameter.name += ' ';
                    parameter.name.append(index, converted.ptr);
                }
            }
        }
        // don't free strings in the callback, keep the removed records and
        // readers only copy the live part
        sizes[back] = count;
        back = middle.exchange(back | dirty, std::memory_order_acq_rel) &
               ~dirty;
    }

    std::vector<PluginParameter> read() {
        std::vector<PluginParameter> result;
        {
            std::scoped_lock lock(readers);
            if (middle.load(std::memory_order_acquire) & dirty)
                front = middle.exchange(front, std::memory_order_acq_rel) &
                        ~dirty;
            const auto &source = buffers[front];
            result.assign(source.begin(), source.begin() + sizes[front]);
        }
        // sort AFTER unlocking
        std::ranges::stable_sort(result, {}, &PluginParameter::id);
        return result;
    }
};

} // namespace ekphore
