// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "execution.h"
#include "../effects/utilities/constructor_random.h"
#include "../effects/utilities/engine_effect_type.h"
#include "../effects/utilities/engine_filter_type.h"
#include "../sampling/audio_file.h"

namespace ekphore::detail {
namespace {

enum class EffectBankLayout { gain, equalizer };

// gain and EQ don't use the same insert/send selectors, so pass the family in
template <class Controls>
auto effect_chain(Controls &controls, std::size_t program, std::int64_t slot,
        std::int64_t selector, EffectBankLayout layout)
        -> decltype(&controls.inserts[program]) {
    const bool send = layout == EffectBankLayout::gain
                              ? selector == 0 || selector == -1
                              : selector == 1;
    if (send) {
        if (program < controls.sends.size() && slot >= 0 && slot < 8)
            return &controls.sends[program][static_cast<std::size_t>(slot)];
        return nullptr;
    }
    const bool insert = layout == EffectBankLayout::gain
                                ? selector == 1
                                : selector == 0 || selector == -1;
    if (insert)
        return program < controls.inserts.size() ? &controls.inserts[program]
                                                 : nullptr;
    if (selector == 2)
        return program < controls.main.size() ? &controls.main[program]
                                              : nullptr;
    if (selector < 0x400 || selector >= 0x410 ||
            program >= controls.buses.size())
        return nullptr;
    const auto bus = static_cast<std::size_t>(selector - 0x400);
    return bus < controls.buses[program].size() ? &controls.buses[program][bus]
                                                : nullptr;
}

} // namespace

ScriptExecutionState::ScriptExecutionState() { bind_trace_value_reader(); }

ScriptExecutionState::ScriptExecutionState(
        const ScriptExecutionState &source, ScriptHostBindings independent)
    : environment(source.environment), engine(source.engine),
      host(source.host, std::move(independent)), effects(source.effects),
      prepared_samples(source.prepared_samples),
      prepared_irs(source.prepared_irs),
      instrument_path(source.instrument_path),
      performance_view_source(source.performance_view_source),
      ir_resource_reader(source.ir_resource_reader) {
    if (source.environment_bound)
        bind_environment();
    if (source.engine_bound)
        bind_engine_state();
    if (source.effect_controls_bound)
        bind_effect_controls();
    if (source.resources_bound)
        bind_resources();
    if (performance_view_source)
        host.performance_view_loader = performance_view_source;
    scripts.reserve(source.scripts.size());
    for (const auto &script : source.scripts)
        scripts.push_back({ script.slot, script.runtime.clone_state(host),
                script.loaded });
    bind_trace_value_reader();
}

void ScriptExecutionState::bind_trace_value_reader() {
    host.script_trace_value_reader =
            [this](std::size_t slot,
                    std::string_view name) -> std::optional<libksp::Value> {
        const auto found =
                std::ranges::find(scripts, slot, &ScriptInstance::slot);
        if (found == scripts.end() || !found->loaded)
            return std::nullopt;
        struct ReadOnlyLookup {
            bool &flag;
            bool previous;
            ~ReadOnlyLookup() { flag = previous; }
        } restore{ host.diagnostic_symbol_read, host.diagnostic_symbol_read };
        host.diagnostic_symbol_read = true;
        return found->runtime.get(name);
    };
}

const EffectControl *ScriptExecutionState::ir_control(
        const IrcEffectState &address) const {
    if (address.group_index)
        return nullptr;
    const EffectControlState::Chain *chain{};
    const auto p = address.program_index;
    if (address.generic_selector == 1 && p < effects.inserts.size())
        chain = &effects.inserts[p];
    else if (address.generic_selector == 20 && p < effects.main.size())
        chain = &effects.main[p];
    else if (address.generic_selector == 0 && p < effects.sends.size() &&
             address.slot_index < 8)
        chain = &effects.sends[p][address.slot_index];
    else if (address.generic_selector >= 3 && address.generic_selector <= 18 &&
             p < effects.buses.size()) {
        const auto bus = static_cast<std::size_t>(address.generic_selector - 3);
        if (bus < effects.buses[p].size())
            chain = &effects.buses[p][bus];
    }
    if (!chain)
        return nullptr;
    const auto found = std::ranges::find(
            *chain, address.slot_index, &EffectControl::slot_index);
    return found != chain->end() &&
                           found->effect_type == libnki::GroupEffectType::irc
                   ? &*found
                   : nullptr;
}

std::filesystem::path ScriptExecutionState::resolve_resource(
        const std::string &requested) const {
    auto path = std::filesystem::path(requested);
    if (path.is_relative() && instrument_path)
        path = instrument_path->parent_path() / path;
    return path.lexically_normal();
}

std::optional<irc::BranchParameters> ScriptExecutionState::ir_branches(
        const IrcEffectState &metadata, const EffectControl &control) {
    if (!control.irc_branches)
        return std::nullopt;
    auto p = *control.irc_branches;
    p.early_stretch = metadata.early_length_ratio;
    p.late_stretch = metadata.late_length_ratio;
    p.early_low_pass_hz = metadata.early_low_pass_frequency;
    p.early_high_pass_hz = metadata.early_high_pass_frequency;
    p.late_low_pass_hz = metadata.late_low_pass_frequency;
    p.late_high_pass_hz = metadata.late_high_pass_frequency;
    p.crosspoint = metadata.early_late_boundary;
    return p;
}

void ScriptExecutionState::bind_resources() {
    resources_bound = true;
    host.set_sample = [this](std::size_t zone_index,
                              const std::string &requested) {
        if (engine.model.instruments.empty() || requested.empty())
            return false;
        auto &instrument = engine.model.instruments.front();
        if (zone_index >= instrument.zones.size())
            return false;
        try {
            const auto path = resolve_resource(requested);
            auto decoded =
                    host.external.observe<std::shared_ptr<const DecodedSample>>(
                            "sample:" + path.string(), [&] {
                                return std::make_shared<const DecodedSample>(
                                        sampling::decode_sample_file(path));
                            });
            libnki::SampleInfo metadata;
            metadata.path = path.string();
            metadata.preferred_name = path.filename().string();
            metadata.sample_rate =
                    static_cast<std::int32_t>(decoded->sample_rate);
            metadata.channel_count =
                    static_cast<std::int32_t>(decoded->channels.size());
            metadata.frame_count =
                    decoded->channels.empty()
                            ? 0
                            : static_cast<std::int32_t>(std::min<std::size_t>(
                                      decoded->channels.front().size(),
                                      std::numeric_limits<
                                              std::int32_t>::max()));
            const auto index = instrument.samples.size();
            // reserve first so nothing throws after the zone is visible
            prepared_samples.reserve(prepared_samples.size() + 1);
            instrument.samples.push_back(std::move(metadata));
            prepared_samples.push_back({ index, std::move(decoded) });
            auto &zone = instrument.zones[zone_index];
            zone.sample_index = index;
            zone.sample_reference_status =
                    libnki::SampleReferenceStatus::resolved;
            if (zone_index < engine.zone_ignored.size())
                engine.zone_ignored[zone_index] = false;
            return true;
        } catch (const ExternalObservations::Divergence &) {
            throw;
        } catch (const std::exception &) { return false; }
    };
    host.load_ir_sample = [this](IrcEffectState &metadata,
                                  const std::string &requested) {
        const auto *control = ir_control(metadata);
        if (requested.empty() || !control)
            return false;
        try {
            const auto path = resolve_resource(requested);
            std::string envelope_key =
                    control->irc_envelope.active ? "active" : "inactive";
            const auto branches = ir_branches(metadata, *control);
            std::string branch_key = "legacy";
            if (branches) {
                branch_key = "authored";
                for (float v : { branches->early_stretch,
                             branches->late_stretch, branches->early_pitch,
                             branches->late_pitch, branches->early_low_pass_hz,
                             branches->early_high_pass_hz,
                             branches->late_low_pass_hz,
                             branches->late_high_pass_hz,
                             branches->crosspoint })
                    branch_key +=
                            ":" +
                            std::to_string(std::bit_cast<std::uint32_t>(v));
                branch_key +=
                        ":" + std::to_string(branches->processing_sample_rate);
                branch_key +=
                        branches->sample_rate_decimation
                                ? ":" + std::to_string(std::bit_cast<
                                                std::uint32_t>(*branches
                                                        ->sample_rate_decimation))
                                : ":missing";
                branch_key +=
                        branches->preserve_ir_length
                                ? (*branches->preserve_ir_length ? ":true"
                                                                 : ":false")
                                : ":missing";
            }
            for (std::size_t point = 0; point < 8; ++point)
                envelope_key += ":" +
                                std::to_string(std::bit_cast<std::uint32_t>(
                                        control->irc_envelope.times[point])) +
                                ":" +
                                std::to_string(std::bit_cast<std::uint32_t>(
                                        control->irc_envelope.levels[point]));
            auto prepared = host.external.observe<
                    std::shared_ptr<const irc::PreparedFile>>(
                    ExternalObservations::key({ "ir",
                            std::to_string(control->irc_convolution_block_size),
                            std::to_string(control->irc_pre_delay_ms),
                            std::to_string(metadata.reverse),
                            std::to_string(metadata.auto_gain),
                            std::to_string(environment.sample_rate),
                            envelope_key, branch_key, path.string(),
                            requested }),
                    [&] {
                        return std::make_shared<const irc::PreparedFile>(
                                irc::prepare_file({ path, requested,
                                        control->irc_convolution_block_size,
                                        metadata.reverse, metadata.auto_gain,
                                        environment.sample_rate,
                                        control->irc_pre_delay_ms,
                                        control->irc_envelope, branches,
                                        ir_resource_reader }));
                    });
            auto address = metadata;
            address.loaded_path = requested;
            address.sample_index.reset();
            auto loaded_path = prepared->requested_path;
            prepared_irs.push_back({ std::move(address),
                    control->reset_revision, prepared->convolver, branches,
                    environment.sample_rate });
            metadata.loaded_path.swap(loaded_path);
            metadata.sample_index.reset();
            return true;
        } catch (const ExternalObservations::Divergence &) {
            throw;
        } catch (const std::exception &) { return false; }
    };
}

void ScriptExecutionState::bind_performance_view_source(
        decltype(ScriptHostBindings::performance_view_loader) source) {
    performance_view_source = std::move(source);
    host.performance_view_loader = performance_view_source;
}

void ScriptExecutionState::bind_engine_state() {
    host.instrument = engine.model.instruments.empty()
                              ? nullptr
                              : &engine.model.instruments.front();
    host.group_purged = &engine.group_purged;
    host.zone_ignored = &engine.zone_ignored;
    host.group_outputs = &engine.group_outputs;
    host.events = &engine.events;
    host.polyphonic_event_indices = &engine.polyphonic_event_indices;
    host.generated_notes = &engine.generated_notes;
    host.voices = &engine.voices;
    host.fades = &engine.fades;
    host.pending_event_fades = &engine.pending_event_fades;
    host.last_event_id = &engine.last_event_id;
    host.irc_effect_states = &engine.irc_effect_states;
    engine_bound = true;
}

void ScriptExecutionState::bind_environment() {
    host.controllers = &environment.controllers;
    host.pitch_bend = &environment.pitch_bend;
    host.channel_pressure = &environment.channel_pressure;
    host.poly_aftertouch = &environment.poly_aftertouch;
    host.sample_rate = &environment.sample_rate;
    host.tempo_bpm = &environment.tempo_bpm;
    host.signature_numerator = &environment.signature_numerator;
    host.signature_denominator = &environment.signature_denominator;
    host.engine_uptime_us = &environment.engine_uptime_us;
    host.engine_elapsed_frames = &environment.engine_elapsed_frames;
    environment_bound = true;
}

void ScriptExecutionState::bind_effect_controls() {
    if (!host.tempo_bpm)
        throw std::invalid_argument(
                "effect controls require an independent tempo binding");
    host.set_engine_parameter = [this](std::size_t program,
                                        std::int64_t parameter,
                                        std::int64_t value, std::int64_t group,
                                        std::int64_t slot,
                                        std::int64_t generic) {
        auto &controls = effects;
        if (slot < 0 || program >= controls.inserts.size())
            return false;
        const auto normalized = [](std::int64_t raw) {
            return std::clamp(static_cast<float>(raw) * 1.0e-6F, 0.0F, 1.0F);
        };
        if (group == -1 && parameter == 113) {
            // gain is Pow3_24 with inserts=1, sends=0/-1, main=2 and buses
            // 400+, don't mix these up with the EQ selectors
            auto *chain = effect_chain(
                    controls, program, slot, generic, EffectBankLayout::gain);
            if (!chain || value < 0 ||
                    value > std::numeric_limits<std::int32_t>::max())
                return false;
            auto effect_it =
                    (generic == 0 || generic == -1)
                            ? chain->begin()
                            : std::ranges::find(*chain, std::size_t(slot),
                                      &EffectControl::slot_index);
            if (effect_it == chain->end() ||
                    effect_it->effect_type != libnki::GroupEffectType::gainer)
                return false;
            const float x = static_cast<float>(
                    double(static_cast<std::int32_t>(value)) * 1.e-6);
            const float square = x * x;
            effect_it->gainer_gain =
                    std::fma(std::clamp(square * x, 0.F, 1.F), 16.F, 0.F);
            effect_it->gainer_revision = ++effect_it->revision;
            return true;
        }
        if (group == -1 &&
                (parameter == 91 || parameter == 92 || parameter == 465 ||
                        (parameter == 12 && generic == 1))) {
            // 12 also hits program inserts with generic 1 but only sets the
            // output, don't guess its other banks from 91. 91/92 use 1 for
            // inserts and 0/-1 for sends, EQ 335 doesn't
            auto *chain = effect_chain(
                    controls, program, slot, generic, EffectBankLayout::gain);
            if (!chain)
                return false;
            auto effect_it =
                    (generic == 0 || generic == -1)
                            ? chain->begin()
                            : std::ranges::find(*chain, std::size_t(slot),
                                      &EffectControl::slot_index);
            if (effect_it == chain->end())
                return false;
            const float x = static_cast<float>(
                    double(static_cast<std::int32_t>(value)) * 1.e-6);
            if (parameter == 465) {
                // 465 is Galois only, no conversion
                if (effect_it->effect_type !=
                        libnki::GroupEffectType::galois_reverb)
                    return false;
                effect_it->galois_pre_delay = x;
                effect_it->galois_pre_delay_revision = ++effect_it->revision;
                return true;
            }
            const float square = x * x;
            const float gain =
                    std::fma(std::clamp(square * x, 0.F, 1.F), 16.F, 0.F);
            if (parameter == 91 || parameter == 12) {
                effect_it->output_target = gain;
                effect_it->output_revision = ++effect_it->revision;
            } else {
                effect_it->dry_target = gain;
                effect_it->dry_revision = ++effect_it->revision;
            }
            return true;
        }
        if (group == -1 && parameter >= 31 && parameter <= 39) {
            // EQ has its own selectors, but buses 400..40f and main 2 still
            // work the same
            auto *chain = effect_chain(controls, program, slot, generic,
                    EffectBankLayout::equalizer);
            if (!chain)
                return false;
            auto effect_it =
                    generic == 1 ? chain->begin()
                                 : std::ranges::find(*chain, std::size_t(slot),
                                           &EffectControl::slot_index);
            if (effect_it == chain->end() ||
                    effect_it->effect_type != libnki::GroupEffectType::filter ||
                    effect_it->type < libnki::FilterType::eq_1_band ||
                    effect_it->type > libnki::FilterType::eq_3_band)
                return false;
            const auto band = std::size_t(parameter - 31) / 3;
            const auto component = (parameter - 31) % 3;
            const float unit = static_cast<float>(
                    double(static_cast<std::int32_t>(value)) * 1.e-6);
            // bandwidth is unipolar but gain is bipolar
            if (component == 0)
                effect_it->eq_frequency[band] = static_cast<float>(std::exp(
                        double(unit) * 6.907755278982137 + 2.995732273553991));
            else if (component == 1)
                effect_it->eq_bandwidth[band] =
                        std::max(unit, 0.F) * 2.6666667F + 0.33333334F;
            else {
                const float bipolar = std::clamp(unit * 2.F - 1.F, -1.F, 1.F);
                effect_it->eq_gain[band] = (bipolar * .5F + .5F) * 36.F - 18.F;
            }
            effect_it->eq_revision = ++effect_it->revision;
            return true;
        }
        if (group == -1 && parameter >= 335 && parameter <= 346) {
            auto *chain = effect_chain(controls, program, slot, generic,
                    EffectBankLayout::equalizer);
            if (!chain)
                return false;
            auto found = generic == 1
                                 ? std::ranges::find(*chain,
                                           libnki::GroupEffectType::solid_g_eq,
                                           &EffectControl::effect_type)
                                 : std::ranges::find(*chain, std::size_t(slot),
                                           &EffectControl::slot_index);
            if (found == chain->end() ||
                    found->effect_type != libnki::GroupEffectType::solid_g_eq)
                return false;
            // scale in double before rounding to float, and no clamp here
            const float unit = static_cast<float>(
                    double(static_cast<std::int32_t>(value)) * 1.e-6);
            auto &p = found->solid_g_eq_parameters;
            switch (parameter) {
                case 335:
                    p.low_gain = unit;
                    break;
                case 336:
                    p.low_frequency = unit;
                    break;
                case 337:
                    p.low_bell = value != 0;
                    break;
                case 338:
                    p.low_mid_gain = unit;
                    break;
                case 339:
                    p.low_mid_frequency = unit;
                    break;
                case 340:
                    p.low_mid_q = unit;
                    break;
                case 341:
                    p.high_mid_gain = unit;
                    break;
                case 342:
                    p.high_mid_frequency = unit;
                    break;
                case 343:
                    p.high_mid_q = unit;
                    break;
                case 344:
                    p.high_gain = unit;
                    break;
                case 345:
                    p.high_frequency = unit;
                    break;
                case 346:
                    p.high_bell = value != 0;
                    break;
            }
            found->solid_g_eq_revision = ++found->revision;
            return true;
        }
        if (group == -1 && generic == 0 && parameter == 276 &&
                program < controls.sends.size() && slot < 8) {
            auto &chain =
                    controls.sends[program][static_cast<std::size_t>(slot)];
            for (auto &effect : chain) {
                effect.bypassed = value != 0;
                effect.bypass_revision = ++effect.revision;
            }
            return !chain.empty();
        }
        if (group == -1 && generic == 0 && parameter >= 79 && parameter <= 86) {
            auto &chain = controls.inserts[program];
            auto effect_it = std::ranges::find(chain,
                    static_cast<std::size_t>(slot), &EffectControl::slot_index);
            if (effect_it == chain.end() ||
                    effect_it->effect_type !=
                            libnki::GroupEffectType::send_levels)
                return false;
            effect_it->send_levels[static_cast<std::size_t>(parameter - 79)] =
                    engine_volume_from_unity(value);
            effect_it->sends_revision = ++effect_it->revision;
            return true;
        }
        if (group == -1 && generic == 0 && program < controls.sends.size() &&
                slot < 8) {
            auto &chain =
                    controls.sends[program][static_cast<std::size_t>(slot)];
            auto effect = std::ranges::find(chain,
                    libnki::GroupEffectType::replika_delay,
                    &EffectControl::effect_type);
            if (effect != chain.end()) {
                auto &p = effect->replika_delay_parameters;
                const float unit = normalized(value);
                switch (parameter) {
                    case 470: {
                        const auto sixteenths = std::clamp(
                                static_cast<int>(
                                        std::llround(1.0 + unit * 11.0)),
                                1, 12);
                        p.time = static_cast<float>(sixteenths) * 15000.0F /
                                 static_cast<float>(
                                         std::max(*host.tempo_bpm, 0.1));
                        break;
                    }
                    case 472:
                        p.feedback = unit;
                        break;
                    case 473:
                        p.low_cut = unit;
                        break;
                    case 474:
                        p.high_cut = unit;
                        break;
                    case 475:
                        p.saturate = unit;
                        break;
                    case 476:
                        p.tape_age = unit;
                        break;
                    case 477:
                        p.flutter = unit;
                        break;
                    case 478:
                        p.quality = unit;
                        break;
                    case 479:
                        p.depth = unit;
                        break;
                    case 480:
                        p.rate = unit;
                        break;
                    case 481:
                        p.style = static_cast<int>(value);
                        break;
                    case 482:
                        p.routing_enabled = value != 0;
                        break;
                    case 483:
                        p.tape_noise_enabled = value != 0;
                        break;
                    case 484:
                        p.routing_variant = value != 0;
                        break;
                    case 485:
                        p.bbd_type = unit;
                        break;
                    case 486:
                        p.diffusion_amount = unit;
                        break;
                    case 487:
                        p.diffusion_size = unit;
                        break;
                    case 488:
                        p.diffusion_mod = unit;
                        break;
                    case 489:
                        p.diffusion_enabled = value != 0;
                        break;
                    case 890:
                        p.routing_variant = value != 0;
                        break;
                    case 891:
                        p.shuffle = unit;
                        break;
                    case 892:
                        p.feel = unit;
                        break;
                    case 893:
                        p.accent = unit;
                        break;
                    case 894:
                        p.pan = unit;
                        break;
                    case 895:
                        p.width = unit;
                        break;
                    case 896:
                        p.lr_offset = unit;
                        break;
                    case 897:
                        p.ducking_sensitivity = unit;
                        break;
                    case 898:
                        p.ducking_release = unit;
                        break;
                    case 899:
                        p.ducking_amount = unit;
                        break;
                    default:
                        return false;
                }
                effect->replika_revision = ++effect->revision;
                return true;
            }
        }
        if (group != -1 || generic != 1)
            return false;
        auto &chain = controls.inserts[program];
        auto found = std::ranges::find(chain, static_cast<std::size_t>(slot),
                &EffectControl::slot_index);
        if (found == chain.end())
            return false;
        if ((parameter == 550 || parameter == 551) &&
                found->effect_type == libnki::GroupEffectType::choral) {
            // clamp here, the DSP builds its coefficients straight from these
            const float unit = std::clamp(
                    static_cast<float>(static_cast<double>(value) * 1.0e-6),
                    0.0F, 1.0F);
            if (parameter == 550)
                found->choral_parameters.feedback = unit;
            else
                found->choral_parameters.mix = unit;
            found->choral_revision = ++found->revision;
            return true;
        }
        if (found->effect_type == libnki::GroupEffectType::flair) {
            std::optional<std::size_t> field;
            switch (parameter) {
                case 530:
                    field = 0;
                    break; // Amount
                case 532:
                    field = 2;
                    break; // free Rate (normalized wire field)
                case 534:
                    field = 4;
                    break; // Mix
                case 538:
                    field = 8;
                    break; // Detune
                default:
                    break;
            }
            if (field) {
                // these Flair fields all use identity conversion
                found->flair_parameters.primary[*field] =
                        static_cast<float>(static_cast<double>(value) * 1.0e-6);
                found->flair_revision = ++found->revision;
                return true;
            }
        }
        if (parameter == 305 &&
                found->effect_type == libnki::GroupEffectType::rotator) {
            // scale in double, then the cubic gain curve
            const float unit =
                    static_cast<float>(static_cast<double>(value) * 1.0e-6);
            const float square = unit * unit;
            found->rotator_parameters.mix = std::max(square * unit, 0.0F);
            found->rotator_revision = ++found->revision;
            return true;
        }
        if (parameter == 246) {
            const auto type = effect_type_from_ksp(value);
            if (!type)
                return false;

            // same type, nothing to remove or insert
            if (found->effect_type == *type)
                return true;
            EffectControl replacement;
            replacement.slot_index = static_cast<std::size_t>(slot);
            replacement.effect_type = *type;
            replacement.revision = found->revision + 1;
            replacement.reset_revision = replacement.revision;
            auto &stream = effect_runtime::constructor_random_owner().stream;
            replacement.constructor_phase =
                    effect_runtime::retained_constructor_phase(
                            *type, true, [&] { return stream.next(); });
            *found = std::move(replacement);
            return true;
        }
        if (parameter == 247 &&
                found->effect_type == libnki::GroupEffectType::filter &&
                value >= 0 &&
                value <= std::numeric_limits<std::uint8_t>::max()) {
            const auto effect_type = found->effect_type;
            const auto slot_index = found->slot_index;
            EffectControl replacement;
            replacement.effect_type = effect_type;
            replacement.type = static_cast<libnki::FilterType>(value);
            replacement.slot_index = slot_index;
            replacement.revision = found->revision + 1;
            replacement.reset_revision = replacement.revision;
            *found = std::move(replacement);
            return true;
        }
        return false;
    };
    host.get_engine_parameter =
            [this](std::size_t program, std::int64_t parameter,
                    std::int64_t group, std::int64_t slot,
                    std::int64_t generic) -> std::optional<std::int64_t> {
        if (parameter == 113 && group == -1 && slot >= 0 &&
                program < effects.inserts.size()) {
            auto *chain = effect_chain(
                    effects, program, slot, generic, EffectBankLayout::gain);
            if (!chain)
                return std::nullopt;
            auto effect_it =
                    (generic == 0 || generic == -1)
                            ? chain->begin()
                            : std::ranges::find(*chain, std::size_t(slot),
                                      &EffectControl::slot_index);
            if (effect_it == chain->end() ||
                    effect_it->effect_type != libnki::GroupEffectType::gainer)
                return std::int64_t{ 0 };

            // use the template value, the raw setter one can be stale
            const float unit = std::pow(
                    std::clamp(effect_it->gainer_gain * .0625F, 0.F, 1.F),
                    std::bit_cast<float>(std::uint32_t{ 0x3eaaaaab }));
            return static_cast<std::int64_t>(std::fma(double(unit), 1.e6, .5));
        }
        if (slot < 0 || group != -1 ||
                (parameter != 246 && parameter != 247 && parameter != 248 &&
                        parameter != 275 && parameter != 276))
            return std::nullopt;
        const auto &controls = effects;
        const EffectControlState::Chain *chain = nullptr;
        if ((parameter == 275 || parameter == 276) && generic == 0 &&
                program < controls.sends.size() && slot < 8) {
            chain = &controls.sends[program][static_cast<std::size_t>(slot)];
        } else if ((parameter == 246 || parameter == 247 || parameter == 248) &&
                   generic >= 1024 && generic < 1040 &&
                   program < controls.buses.size()) {
            const auto bus = static_cast<std::size_t>(generic - 1024);
            if (bus < controls.buses[program].size())
                chain = &controls.buses[program][bus];
        } else if ((parameter == 246 || parameter == 247 || parameter == 248) &&
                   generic == 2 && program < controls.main.size()) {
            chain = &controls.main[program];
        } else if ((parameter == 246 || parameter == 247 || parameter == 248) &&
                   generic == 1 && program < controls.inserts.size()) {
            chain = &controls.inserts[program];
        }
        if (!chain)
            return std::nullopt;
        const auto effect = std::ranges::find(*chain,
                static_cast<std::size_t>(slot), &EffectControl::slot_index);
        if (effect == chain->end())
            return std::int64_t{ 0 };
        if (parameter == 248 || parameter == 276)
            return static_cast<std::int64_t>(effect->bypassed);
        if (parameter == 247)
            return effect->effect_type == libnki::GroupEffectType::filter
                           ? ksp_filter_type(effect->filter_subtype.value_or(
                                     static_cast<std::uint32_t>(effect->type)))
                           : 0;
        return ksp_effect_type(effect->effect_type).value_or(0);
    };
    effect_controls_bound = true;
}

bool apply_external_script_source(
        libnki::KspScriptInfo &script, std::span<const std::byte> bytes) {
    std::string source;
    if (!bytes.empty())
        source.assign(
                reinterpret_cast<const char *>(bytes.data()), bytes.size());
    // stop at NUL and make sure we end with a LF, kontakt does the same
    if (const auto nul = source.find('\0'); nul != std::string::npos)
        source.resize(nul);
    if (source.empty() || source.back() != '\n')
        source.push_back('\n');
    libksp::Runtime parser;
    if (!parser.load(source))
        return false;
    script.source_text = std::move(source);
    return true;
}

} // namespace ekphore::detail
