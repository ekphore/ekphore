// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dependencies.h"

namespace ekphore::player_detail {
using namespace detail;

struct ModulationState {
    struct ExternalValueChange {
        std::size_t sample_offset{};
        float value{};
    };
    struct ExternalModRuntime {
        struct AssignmentHost final : libksp::Host {
            double input{};
            double output{};
            double signal{};
            double intensity{};
            bool inverted{};
            bool has_tail{ true };
            std::vector<double> table;

            std::optional<libksp::EnvironmentSymbol> resolve_symbol(
                    std::string_view name) override {
                if (name == "$in")
                    return libksp::EnvironmentSymbol{ input, false, false, {} };
                if (name == "$out")
                    return libksp::EnvironmentSymbol{ output, true, false, {} };
                if (name == "$signal")
                    return libksp::EnvironmentSymbol{ signal, false, false,
                        {} };
                if (name == "$intensity")
                    return libksp::EnvironmentSymbol{ intensity, false, false,
                        {} };
                if (name == "$c")
                    return libksp::EnvironmentSymbol{ table, true, false, {} };
                return std::nullopt;
            }

            bool assign_symbol(std::string_view name,
                    const libksp::Value &value) override {
                if (name == "$out") {
                    output = std::get<double>(value);
                    return true;
                }
                if (name == "$c") {
                    if (const auto values =
                                    std::get_if<std::vector<double>>(&value)) {
                        table = *values;
                        return true;
                    }
                }
                return false;
            }

            std::optional<libksp::Value> call(std::string_view name,
                    std::span<const libksp::Value> arguments) override {
                if (name == "make_table" && arguments.size() == 1) {
                    table.assign(129, 0.0);
                    return std::int64_t{ 0 };
                }
                const auto real = [](const libksp::Value &value) {
                    if (const auto number = std::get_if<double>(&value))
                        return *number;
                    if (const auto number = std::get_if<std::int64_t>(&value))
                        return static_cast<double>(*number);
                    return 0.0;
                };
                if (arguments.size() == 1) {
                    const double value = real(arguments[0]);
                    if (name == "has_tail") {
                        // same as truncating but no UB on inf/NaN
                        has_tail = static_cast<float>(value) >= 1.0F;
                        return std::int64_t{ 0 };
                    }
                    if (name == "fcubic")
                        return value * value * value;
                    if (name == "frange_bi")
                        return value * 2.0 - 1.0;
                    if (name == "frange_uni")
                        return value * 0.5 + 0.5;
                    if (name == "fabs")
                        return std::abs(value);
                    if (name == "fcinv_flip")
                        return inverted ? 1.0 - value : value;
                    if (name == "fcinv_neg")
                        return inverted ? -value : value;
                    const float input_value = static_cast<float>(value);
                    if (name == "fconv_u2n_env_a")
                        // clang-format off
                        return static_cast<double>(std::exp(std::max(
                                input_value, 0.0F) * 8.922792F + 0.6931472F) -
                                2.0F);
                    // clang-format on
                    if (name == "fconv_n2u_env_a")
                        return static_cast<double>(std::max(
                                (std::log(input_value + 2.0F) - 0.6931472F) /
                                        8.922792F,
                                0.0F));
                    if (name == "fconv_u2n_env_d")
                        // clang-format off
                        return static_cast<double>(std::exp(std::max(
                                input_value, 0.0F) * 9.433565F + 0.6931472F) -
                                2.0F);
                    // clang-format on
                    if (name == "fconv_n2u_env_d")
                        return static_cast<double>(std::max(
                                (std::log(input_value + 2.0F) - 0.6931472F) /
                                        9.433565F,
                                0.0F));
                }
                if (name == "ftable" && arguments.size() == 2 &&
                        table.size() >= 129) {
                    const double position =
                            std::max(real(arguments[1]), 0.0) * 127.0;
                    const auto index = static_cast<std::size_t>(position);
                    if (index >= table.size())
                        return table.back();
                    const double fraction =
                            position - static_cast<double>(index);
                    const double delta =
                            index < 128 ? table[index + 1] - table[index] : 0.0;
                    return table[index] + fraction * delta;
                }
                return std::nullopt;
            }
        };
        struct AssignmentRuntime {
            AssignmentHost host;
            libksp::Runtime runtime{ &host };
        };
        struct TargetRuntime {
            dsp::ExternalTargetClassification classification;
            std::vector<float> table;
            std::unique_ptr<AssignmentRuntime> assignment;
            bool smoothing_initialized{};
            bool has_tail{ true };
            float smoothed{};
            std::int64_t ramp_remaining{};
            float ramp_delta{};
            // MEQ only reads element 0 even when it splits into 32 frame chunks
            bool eq_block_ready{};
            float eq_block_signal{};

            float smooth(float signal, std::int64_t smoothing,
                    std::uint32_t sample_rate, bool advance) {
                if (!smoothing_initialized) {
                    smoothing_initialized = true;
                    smoothed = signal;
                } else if (smoothing > 0 && advance) {
                    // compute alpha in double before rounding to float
                    const double samples =
                            (static_cast<double>(smoothing) / 1000.0) *
                            static_cast<double>(
                                    static_cast<float>(sample_rate)) /
                            32.0;
                    const float alpha =
                            samples > 0.0
                                    ? static_cast<float>(
                                              1.0 -
                                              std::exp(-4.605170185988092 /
                                                       samples))
                                    : 1.0F;
                    const float difference =
                            dsp::modulation_plane_detail::sub(signal, smoothed);
                    has_tail = !(std::abs(difference) < 1.0e-5F);
                    if (has_tail)
                        smoothed = dsp::modulation_plane_detail::add(
                                smoothed, dsp::modulation_plane_detail::mul(
                                                  difference, alpha));
                } else if (smoothing < 0 && advance) {
                    const float samples =
                            (static_cast<float>(-smoothing) / 1000.0F) *
                            static_cast<float>(sample_rate) * 0.03125F;
                    if (std::abs(signal - smoothed) < 1.0e-5F) {
                        ramp_remaining = 0;
                        ramp_delta = 0.0F;
                    } else {
                        if (ramp_remaining < 1) {
                            ramp_remaining = std::max<std::int64_t>(
                                    static_cast<std::int64_t>(
                                            samples * 0.25F + 0.5F),
                                    1);
                            ramp_delta = (signal - smoothed) /
                                         static_cast<float>(ramp_remaining);
                        }
                        --ramp_remaining;
                        smoothed += ramp_delta;
                    }
                } else if (smoothing == 0) {
                    smoothed = signal;
                }
                return smoothed;
            }
        };
        // indices, not pointers, the script/model storage can be replaced
        std::size_t group_index{}, modulator_index{};
        const libnki::ExternalModulatorInfo *definition(
                const libnki::Model &model) const noexcept {
            if (model.instruments.empty())
                return nullptr;
            const auto &groups = model.instruments.front().groups;
            if (group_index >= groups.size())
                return nullptr;
            const auto &modulators = groups[group_index].external_modulators;
            return modulator_index < modulators.size()
                           ? &modulators[modulator_index]
                           : nullptr;
        }
        float value{};
        // only pitch and EQ use the whole plane for now, the rest still goes
        // sample by sample
        ksp::ExternalControlQueue pitch_source;
        std::array<float, 32> pitch_source_controls{};
        // once per span even if several targets use it
        bool source_controls_generated{};
        float source_controls_previous{};
        std::size_t source_controls_pending{};
        void generate_source_controls(std::size_t count) {
            if (source_controls_generated)
                return;
            source_controls_previous = pitch_source.current();
            source_controls_pending = pitch_source.pending().size();
            (void)pitch_source.generate(
                    std::span(pitch_source_controls).first(count));
            source_controls_generated = true;
        }
        std::vector<ExternalValueChange> changes;
        std::vector<TargetRuntime> targets;
    };
    std::unordered_map<std::uint64_t, std::vector<ExternalModRuntime>>
            external_modulators;
    struct AmpEnvelopeRuntime {
        std::optional<dsp::PlainAhdsr> raw_generator;
        std::uint32_t raw_sample_rate{};
        bool raw_source_rejected{};
        std::size_t modulator_index{};
        std::optional<std::size_t> shared_source;
        std::size_t target_ordinal{};
        std::vector<std::pair<std::size_t, std::size_t>> incoming_intensity;
        std::vector<std::array<float, 32>> resolved_amounts;
        std::optional<std::pair<std::size_t, std::size_t>> shared_lfo_target;
        std::optional<std::pair<std::size_t, std::size_t>> shared_simple_target;
        std::optional<std::pair<std::size_t, std::size_t>>
                shared_external_target;
        std::optional<std::pair<std::size_t, std::size_t>> shared_zone_target;
        bool external_volume_current{};
        int execution_rank{ 80 };
        std::optional<dsp::Fm7Envelope> fm7;
        std::optional<dsp::PlainDbd> dbd;
        // modulate at zero, reset, THEN clear the flag before generating
        dsp::Fm7Envelope::PointInputs fm7_initial_inputs;
        std::array<float, 5> ahdsr_default_planes{};
        float attack_curving{};
        float attack_ms{};
        float hold_ms{};
        float decay_ms{};
        float sustain{ 1.0F };
        float release_ms{};
        bool note_off_less{};
        bool initialized{};
        std::uint8_t stage{ 3 };
        std::int64_t stage_remaining{};
        float shape{ 1.075F };
        float coefficient{ 1.0F };
        float offset{};
        float scale{ 1.0F };
        float value{ 1.0F };
        float release_origin{ 1.0F };
        bool release_started{};
        float target_amount{ 1.0F };
        bool target_inverted{};
        std::vector<float> target_table;
        bool direct_volume{};
        bool retires_voice{};
        std::uint8_t modulation_phase{};
        bool interpolation_initialized{};
        bool interpolation_partial{ true };
        std::optional<bool> interpolation_short_span;
        float interpolation_previous{};
        float interpolation_current{};
        dsp::AhdsrReleaseCountdown release_countdown;
        std::array<float, 32> control_values{};
        std::array<float, 32> raw_controls{};
        std::array<bool, 32> control_finished{};
        std::size_t control_read_index{};
        bool interpolation_finished{};
    };
    std::unordered_map<std::uint64_t, std::vector<AmpEnvelopeRuntime>>
            amp_envelopes;
    struct ZoneEnvelopeRuntime {
        std::size_t modulator_index{};
        std::vector<dsp::ZoneEnvelope::Point> points;
        dsp::ZoneEnvelope generator;
        dsp::ZoneEnvelope::Loop loop;
        bool speed_lock{};
        bool initialized{};
        bool rejected{};
        std::array<float, 32> raw_controls{};
        ZoneEnvelopeRuntime() = default;
        ZoneEnvelopeRuntime(ZoneEnvelopeRuntime &&) = default;
        ZoneEnvelopeRuntime &operator=(ZoneEnvelopeRuntime &&) = default;
        ZoneEnvelopeRuntime(const ZoneEnvelopeRuntime &) = delete;
        ZoneEnvelopeRuntime &operator=(const ZoneEnvelopeRuntime &) = delete;
    };
    std::unordered_map<std::uint64_t, std::vector<ZoneEnvelopeRuntime>>
            zone_envelopes;
    static bool plain_zone_volume(const libnki::ExternalModulatorInfo &source,
            const libnki::ModulationTargetInfo &route) {
        return source.type == libnki::ExternalModulatorType::zone_envelope &&
               source.bypass == false &&
               source.simulate_k2_env_time_modulation == false &&
               (!source.source || static_cast<unsigned>(*source.source) < 7) &&
               (route.id == 2 || route.name == "volume") && route.amount &&
               std::isfinite(*route.amount) && route.inverted.has_value() &&
               !route.assignment &&
               (route.transformation_kind == 0 ||
                       route.transformation_enabled == false) &&
               route.smoothing_coefficient == 0 &&
               (!route.modulator_index || *route.modulator_index < 0);
    }
    static bool plain_velocity_volume(
            const libnki::ExternalModulatorInfo &source,
            const libnki::ModulationTargetInfo &route) {
        return source.type == libnki::ExternalModulatorType::external &&
               source.source == libnki::ExternalModulatorSource::velocity &&
               source.delay == 0.F && source.bypass == false &&
               (route.id == 2 || route.name == "volume") && !route.assignment &&
               !route.transformation_enabled.value_or(false) &&
               route.smoothing_coefficient == 0 && route.slot_index == -1 &&
               route.modulator_index == -1 && route.amount &&
               std::isfinite(*route.amount) && route.native_flags &&
               route.inverted && (*route.native_flags & ~0x12U) == 0 &&
               *route.inverted == ((*route.native_flags & 2U) != 0);
    }
    // same generator, but hitting zero on pitch must NOT kill the voice
    std::unordered_map<std::uint64_t, std::vector<AmpEnvelopeRuntime>>
            pitch_envelopes;
    // shared, so we don't advance copies of the same envelope
    std::unordered_map<std::uint64_t, std::vector<AmpEnvelopeRuntime>>
            raw_envelopes;
    struct MultiTemplate {
        detail::MultiTemplateClockOwner clock;
        detail::MultiTemplateRandomPair initial;
        dsp::MultiLfoSharedTemplate shared;
        dsp::ChaoticLfoSharedTemplate chaotic_shared;
        std::uint32_t chaotic_reset_entropy{};
        libnki::InternalModulatorInfo shape;
        std::uint64_t generation{};
        bool rejected{};
    };
    std::vector<std::array<std::optional<MultiTemplate>, 16>> multi_templates;
    std::uint64_t next_multi_template_generation{ 1 };
    bool multi_templates_published{};
    std::uint64_t multi_templates_validated_revision{};
    static bool same_multi_template_shape(
            const libnki::InternalModulatorInfo &a,
            const libnki::InternalModulatorInfo &b) {
        if (!a.lfo || !b.lfo || a.lfo->waveform != b.lfo->waveform ||
                a.source_slot != b.source_slot || a.retrigger != b.retrigger ||
                a.targets.size() != b.targets.size())
            return false;
        for (std::size_t i = 0; i < a.targets.size(); ++i) {
            const auto &x = a.targets[i];
            const auto &y = b.targets[i];
            // amounts and names don't make it a different envelope
            if (x.id != y.id || x.name != y.name ||
                    x.modulator_index != y.modulator_index ||
                    x.slot_index != y.slot_index ||
                    x.assignment != y.assignment ||
                    x.transformation_enabled != y.transformation_enabled ||
                    x.transformation_kind != y.transformation_kind ||
                    x.transformation_values != y.transformation_values ||
                    x.smoothing_coefficient != y.smoothing_coefficient ||
                    x.native_flags != y.native_flags ||
                    x.inverted != y.inverted)
                return false;
        }
        return true;
    }
    // needs the engine lock, workers can't touch this
    void publish_multi_templates(ScriptExecutionState &scripts);
    MultiTemplate *multi_template(std::size_t group,
            const libnki::InternalModulatorInfo &definition) {
        if (group >= multi_templates.size() || !definition.source_slot ||
                *definition.source_slot < 0 || *definition.source_slot >= 16)
            return nullptr;
        auto &entry = multi_templates[group][*definition.source_slot];
        return entry && !entry->rejected &&
                               same_multi_template_shape(
                                       entry->shape, definition)
                       ? &*entry
                       : nullptr;
    }
    struct PitchLfoRuntime {
        dsp::MultiLfo generator;
        std::optional<dsp::ChaoticLfo> chaotic;
        std::uint32_t chaotic_reset_entropy{};
        std::size_t group_index{}, modulator_index{};
        bool initialized{};
        std::uint64_t template_generation{};
        int execution_rank{ 80 };
        // these are source_slot, NOT the vector index
        std::vector<std::pair<std::size_t, std::size_t>> intensity_routes;
        std::vector<std::pair<std::size_t, std::size_t>> frequency_routes;
        // vector index plus target ordinal
        std::vector<std::pair<std::size_t, std::size_t>> internal_routes;
        std::vector<std::pair<std::size_t, std::size_t>>
                zero_frequency_internal;
        std::vector<std::pair<std::size_t, std::size_t>>
                zero_frequency_external;
        std::array<float, 32> controls{};
        std::array<float, 32> source_controls{};
        bool source_controls_current{};
        // allocated when the voice starts, rendering must not resize it
        std::vector<std::array<float, 32>> target_amounts;
    };
    std::unordered_map<std::uint64_t, std::vector<PitchLfoRuntime>> pitch_lfos;
    struct OrdinaryTemplate {
        dsp::SimpleLfoSharedTemplate cache;
        libnki::InternalModulatorInfo shape;
        std::uint32_t sample_rate{};
        bool initialized{}, rejected{};
    };
    // these outlive the voices and the state is shared, don't fake it with
    // clocks or seeds
    std::map<std::pair<std::size_t, std::int32_t>, OrdinaryTemplate>
            ordinary_templates;
    struct OrdinaryLfoRuntime {
        dsp::SimpleLfo generator;
        // retriggered sources keep their own phase
        dsp::SimpleLfoLocalCache local_cache;
        std::size_t group_index{}, modulator_index{};
        bool initialized{}, current{};
        std::vector<dsp::SimpleLfoPositiveSmoothing> smoothing;
        std::vector<std::array<float, 32>> targets;
        std::vector<std::pair<std::size_t, std::size_t>> frequency_routes;
    };
    struct OrdinaryPanRuntime {
        std::array<float, 32> controls{};
        float previous{}, current{};
        bool initialized{}, partial{ true }, published{};
        std::uint8_t phase{};
    };
    std::unordered_map<std::uint64_t, std::vector<OrdinaryLfoRuntime>>
            ordinary_lfos;
    std::unordered_map<std::uint64_t, OrdinaryPanRuntime> ordinary_pan;
    static bool plain_ordinary_target(
            const libnki::ModulationTargetInfo &route) {
        const bool volume = route.id == 2 || route.name == "volume";
        const bool pan = route.id == 3 || route.name == "pan";
        const bool pitch = route.id == 6 || route.name == "pitch";
        const bool cutoff = route.id == 31 || route.name == "filterCutoff";
        return (volume || pan || pitch || cutoff) && route.amount &&
               std::isfinite(*route.amount) && !route.assignment &&
               !route.transformation_enabled.value_or(false) &&
               route.slot_index == -1 &&
               (cutoff ? (route.modulator_index && *route.modulator_index >= 0)
                       : (!route.modulator_index ||
                                 route.modulator_index == -1)) &&
               route.native_flags && route.inverted &&
               (*route.native_flags & ~(volume ? 0x16U : 0x12U)) == 0 &&
               *route.inverted == ((*route.native_flags & 2U) != 0) &&
               route.smoothing_coefficient &&
               *route.smoothing_coefficient >= 0 &&
               (!(volume || cutoff) || *route.smoothing_coefficient == 0);
    }
    static bool same_ordinary_coefficients(
            const libnki::InternalModulatorInfo &a,
            const libnki::InternalModulatorInfo &b) {
        if (!same_multi_template_shape(a, b) || a.bypass != b.bypass)
            return false;
        const auto &x = *a.lfo;
        const auto &y = *b.lfo;
        if (x.frequency != y.frequency || x.delay != y.delay ||
                x.start_phase != y.start_phase ||
                x.pulse_width != y.pulse_width ||
                x.serialized_flag_1d != y.serialized_flag_1d ||
                x.note_value_frequency != y.note_value_frequency ||
                x.note_value_delay != y.note_value_delay ||
                x.note_value_frequency_enabled !=
                        y.note_value_frequency_enabled ||
                x.note_value_delay_enabled != y.note_value_delay_enabled)
            return false;
        for (std::size_t i = 0; i < a.targets.size(); ++i)
            if (a.targets[i].amount != b.targets[i].amount)
                return false;
        return true;
    }
    // only for one span, doesn't own anything
    struct EnvelopeStepper {
        ModulationState &modulation;
        detail::ScriptHost &host;
        const VoiceInfo &voice;
        const libnki::GroupInfo *source_group;
        decltype(player_detail::ModulationState::external_modulators)::iterator
                external;
        std::size_t active_frames;
        std::uint32_t sample_rate;
        bool wait_for_streaming;
        struct {
            bool &offline_sample_data_incomplete;
        } output;
        template <class Route>
        static bool ahdsr_time_target(const Route &route) {
            return (route.id && *route.id >= 0xb7 && *route.id <= 0xc0) ||
                   (route.name && route.name->starts_with("ahdsr_"));
        }
        std::int64_t envelope_samples(float milliseconds);
        bool modulate_ahdsr(AmpEnvelopeRuntime &envelope, bool release_only);
        void enter_decay_or_sustain(
                AmpEnvelopeRuntime &envelope, bool from_transition);
        void enter_hold_decay_or_sustain(
                AmpEnvelopeRuntime &envelope, bool from_transition);
        void initialize_envelope(AmpEnvelopeRuntime &envelope);
        void enter_release(AmpEnvelopeRuntime &envelope);
        float target_gain(const AmpEnvelopeRuntime &envelope, float raw_signal,
                float amount);
        void advance_envelope_control(AmpEnvelopeRuntime &envelope);
    };

    struct VoiceBindings {
        decltype(external_modulators)::iterator external;
        decltype(amp_envelopes)::iterator amp_envelope;
        decltype(pitch_envelopes)::iterator pitch_envelope;
        decltype(raw_envelopes)::iterator raw_envelope;
        decltype(pitch_lfos)::iterator pitch_lfo;
        decltype(ordinary_lfos)::iterator ordinary_lfo;
        decltype(ModulationState::ordinary_pan)::iterator ordinary_pan;
    };
    struct ControlSpan {
        const libnki::Model &model;
        std::size_t voice_start, release_offset;
        std::uint64_t source_stamp;
        double tempo;
    };
    struct AudioSpan {
        EnvelopeStepper &stepper;
        const libnki::Model &model;
        const VoiceBindings &bindings;
        std::vector<effect_runtime::EffectRuntime> *effects;
        std::size_t voice_start, frame_count;
        bool wavetable_source;
        std::size_t pan_control_index{};
    };
    std::pair<float, float> consume_voice_controls(
            AudioSpan &span, std::size_t frame, std::uint8_t pitch_phase) {
        auto *effects = span.effects;
        if (effects) {
            for (auto &effect : *effects) {
                effect.voice_filter_cutoff = effect.cutoff;
                effect.voice_filter_cutoff_attached = false;
                effect.voice_dyx_morph = effect.dyx_morph;
                effect.voice_dyx_dry = effect.dyx_dry;
                effect.voice_send_modulation.fill(1.0F);
                effect.voice_eq_modulated.fill(false);
                effect.voice_eq_component_masks.fill(0);
                if (effect.effect_type == libnki::GroupEffectType::filter &&
                        effect.type >= libnki::FilterType::eq_1_band &&
                        effect.type <= libnki::FilterType::eq_3_band) {
                    for (std::size_t band = 0; band < 3; ++band)
                        effect.voice_eq_values[band] =
                                parametric_eq::Filter::normalize(
                                        effect.eq_frequency[band].value_or(
                                                20.0F),
                                        effect.eq_bandwidth[band].value_or(
                                                0.33333334F),
                                        effect.eq_gain[band].value_or(0.0F));
                }
            }
        }
        float gain = 1.0F, pan = 0.0F;
        const auto &bindings = span.bindings;
        if (bindings.external != external_modulators.end())
            consume_external_controls(span, frame, pitch_phase, gain, pan);
        if ((effects &&
                    (bindings.raw_envelope != raw_envelopes.end() ||
                            bindings.ordinary_lfo != ordinary_lfos.end())) ||
                (bindings.ordinary_pan != ordinary_pan.end() &&
                        bindings.ordinary_pan->second.published))
            consume_internal_controls(span, frame, pan);
        return { gain, pan };
    }
    void generate_voice_controls(EnvelopeStepper &stepper,
            const ControlSpan &span, const VoiceBindings &bindings);
    void prepare_external_eq_controls(const libnki::Model &model,
            std::span<ExternalModRuntime> runtimes, std::size_t active_frames,
            std::uint32_t sample_rate);

  private:
    void consume_external_controls(AudioSpan &span, std::size_t frame,
            std::uint8_t pitch_phase, float &modulation_gain,
            float &modulation_pan);
    void consume_internal_controls(
            AudioSpan &span, std::size_t frame, float &modulation_pan);
    void generate_envelopes(EnvelopeStepper &stepper, const ControlSpan &span,
            const VoiceBindings &bindings,
            std::vector<AmpEnvelopeRuntime> &envelopes, bool pitch_target);
    void generate_raw_envelopes(EnvelopeStepper &stepper,
            const ControlSpan &span, const VoiceBindings &bindings);
    void generate_multi_lfos(EnvelopeStepper &stepper, const ControlSpan &span,
            const VoiceBindings &bindings);
    void generate_ordinary_lfos(EnvelopeStepper &stepper,
            const ControlSpan &span, const VoiceBindings &bindings);
    void publish_shared_controls(EnvelopeStepper &stepper,
            const ControlSpan &span, const VoiceBindings &bindings);
};

} // namespace ekphore::player_detail
