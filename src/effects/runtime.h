// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tail_activity.h"

// include the DSP headers before this

namespace ekphore::effect_runtime {

struct EffectRuntime {
    std::uint64_t script_configuration_revision{};
    // once per block, modulation targets get reset every sample
    bool voice_gainer_active{};
    bool voice_output_smoothing{};
    bool voice_dry_smoothing{};
    float voice_filter_cutoff{};
    bool voice_filter_cutoff_attached{};
    float voice_dyx_morph{};
    float voice_dyx_dry{};

    using SampleProcessor = void (*)(
            EffectRuntime &, std::uint32_t, float, float &, float &);

    struct DyxTransferRuntime {
        std::size_t destination_zone{};
        std::array<float, 5> bounds{};
        std::array<dyx::ResponseSpectrum, 2> channels;
        std::array<dyx::StreamingConvolver, 2> convolvers;
    };
    libnki::GroupEffectType effect_type{ libnki::GroupEffectType::filter };
    SampleProcessor sample_processor{};
    libnki::FilterType type{ libnki::FilterType::low_pass_2_pole };
    std::size_t slot_index{};
    bool bypassed{};
    float output_target{ 1.0F };
    float output_current{ 1.0F };
    float dry_target{};
    float dry_current{};
    bool supports_dry_wet{ true };
    std::optional<std::size_t> irc_sample_index;
    std::size_t irc_convolution_block_size{};
    float irc_pre_delay_ms{};
    bool irc_auto_gain{};
    bool irc_reverse{};
    irc::Envelope irc_envelope;
    std::optional<irc::BranchParameters> irc_branches;
    std::uint32_t irc_prepared_sample_rate{};
    irc::PairedConvolver irc_convolver;
    irc::DryAlignmentDelay irc_dry_alignment;
    std::int64_t irc_send_tail_remaining{};
    // don't clear this with the scratch, we check the output even without
    // input
    bool insert_has_signal{};
    std::int64_t insert_irc_tail_remaining{};
    tail_activity::RetainedSignal insert_core_activity;
    // two, otherwise switching engines loses the other one's tail
    std::array<tail_activity::RetainedSignal, 2> insert_galois_activity;
    bool inverter_phase{};
    bool inverter_panorama{};
    float stereo_spread_target{ 0.5F };
    float stereo_spread_current{ 0.5F };
    float stereo_pan_target{};
    float stereo_pan_current{};
    bool stereo_pseudo{};
    std::array<float, 1024> stereo_delay{};
    std::uint32_t stereo_delay_index{};
    float shaper_shape{};
    libnki::ShaperModel shaper_model{ libnki::ShaperModel::classic };
    std::uint32_t shaper_sample_rate{};
    float shaper_detector{ 1.0F };
    float shaper_gain{ 1.0F };
    float shaper_detector_release{};
    float shaper_gain_smoothing{};
    float lo_fi_bits{ 1.0F };
    float lo_fi_down_sample{ 1.0F };
    std::span<const float> lo_fi_bitdepth_plane;
    std::span<const float> lo_fi_downsample_plane;
    std::uint32_t lo_fi_control_countdown{};
    std::size_t lo_fi_next_control_index{};
    std::size_t lo_fi_span_remaining{};
    std::size_t lo_fi_chunk_remaining{};
    bool lo_fi_sample_hold_integer{};
    float lo_fi_noise_level{};
    float lo_fi_noise_color{};
    bool lo_fi_down_sample_requires_engine_rate_conversion{};
    std::uint32_t lo_fi_sample_rate{};
    bool lo_fi_bits_active{};
    bool lo_fi_down_sample_active{};
    float lo_fi_quantize_scale{ 1.0F };
    float lo_fi_quantize_inverse{ 1.0F };
    float lo_fi_hold_increment{ 1.0F };
    float lo_fi_hold_phase{};
    std::array<float, 2> lo_fi_held{};
    std::array<float, 2> lo_fi_noise_state{};
    std::array<std::uint32_t, 2> lo_fi_noise_channel_rng{};
    std::size_t lo_fi_noise_chunk_remaining{};
    std::uint32_t lo_fi_random_state{ static_cast<std::uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) };
    libnki::DistortionModel distortion_model{ libnki::DistortionModel::tube };
    float distortion_drive{};
    float distortion_damping{};
    legacy_distortion::Processor distortion_processor;
    libnki::CompressorModel compressor_model{
        libnki::CompressorModel::classic
    };
    float compressor_threshold{};
    float compressor_ratio{ 1.0F };
    float compressor_attack{ 1.0F };
    float compressor_release{ 100.0F };
    bool compressor_stereo_link{};
    compressor::ClassicProcessor compressor_classic;
    compressor::EnhancedProcessor compressor_enhanced;
    compressor::ProProcessor compressor_pro;
    compressor::SolidBusParameters solid_bus_parameters;
    compressor::SolidBusProcessor solid_bus_processor;
    std::uint32_t solid_bus_sample_rate{};
    compressor::FeedbackParameters feedback_compressor_parameters;
    compressor::FeedbackProcessor feedback_compressor_processor;
    std::uint32_t feedback_compressor_sample_rate{};
    supercharger::Parameters supercharger_parameters;
    supercharger::Processor supercharger_processor;
    std::uint32_t supercharger_sample_rate{};
    flair::Parameters flair_parameters;
    flair::Processor flair_processor;
    std::uint32_t flair_sample_rate{};
    float flair_tempo_bpm{};
    cry_wah::Parameters cry_wah_parameters;
    cry_wah::Processor cry_wah_processor;
    std::uint32_t cry_wah_sample_rate{};
    cat::Parameters cat_parameters;
    cat::Processor cat_processor;
    std::uint32_t cat_sample_rate{};
    ac_box::Parameters ac_box_parameters;
    ac_box::Processor ac_box_processor;
    std::uint32_t ac_box_sample_rate{};
    bass_pro::Parameters bass_pro_parameters;
    bass_pro::Processor bass_pro_processor;
    std::uint32_t bass_pro_sample_rate{};
    twin_delay::Parameters twin_delay_parameters;
    twin_delay::Processor twin_delay_processor;
    std::uint32_t twin_delay_sample_rate{};
    float twin_delay_tempo_bpm{};
    raum::Parameters raum_parameters;
    raum::Processor raum_processor;
    std::uint32_t raum_sample_rate{};
    float raum_tempo_bpm{};
    bite::Parameters bite_parameters;
    bite::Processor bite_processor;
    std::uint32_t bite_sample_rate{};
    dirt::Parameters dirt_parameters;
    dirt::Processor dirt_processor;
    std::uint32_t dirt_sample_rate{};
    freak::Parameters freak_parameters;
    freak::Processor freak_processor;
    std::uint32_t freak_sample_rate{};
    vibrato_chorus::Parameters vibrato_chorus_parameters;
    vibrato_chorus::Processor vibrato_chorus_processor;
    std::uint32_t vibrato_chorus_sample_rate{};
    wow_flutter::Parameters wow_flutter_parameters;
    wow_flutter::Processor wow_flutter_processor;
    std::uint32_t wow_flutter_sample_rate{};
    ep_preamps::Parameters ep_preamps_parameters;
    ep_preamps::Processor ep_preamps_processor;
    std::uint32_t ep_preamps_sample_rate{};
    fuzz::Parameters fuzz_parameters;
    fuzz::Processor fuzz_processor;
    std::uint32_t fuzz_sample_rate{};
    fuzz::BigParameters big_fuzz_parameters;
    fuzz::BigProcessor big_fuzz_processor;
    std::uint32_t big_fuzz_sample_rate{};
    stereo_tune::Parameters stereo_tune_parameters;
    stereo_tune::Processor stereo_tune_processor;
    std::uint32_t stereo_tune_sample_rate{};
    beat_masher::Parameters beat_masher_parameters;
    beat_masher::Processor beat_masher_processor;
    std::uint32_t beat_masher_sample_rate{};
    float beat_masher_tempo_bpm{};
    beat_slicer::Parameters beat_slicer_parameters;
    beat_slicer::Processor beat_slicer_processor;
    std::uint32_t beat_slicer_sample_rate{};
    float beat_slicer_tempo_bpm{};
    gater::Parameters gater_parameters;
    gater::Processor gater_processor;
    std::uint32_t gater_sample_rate{};
    float gater_tempo_bpm{};
    reverse_grain::Parameters reverse_grain_parameters;
    reverse_grain::Processor reverse_grain_processor;
    std::uint32_t reverse_grain_sample_rate{};
    float reverse_grain_tempo_bpm{};
    transpose_stretch::Parameters transpose_stretch_parameters;
    transpose_stretch::Processor transpose_stretch_processor;
    std::uint32_t transpose_stretch_sample_rate{};
    float transpose_stretch_tempo_bpm{};
    super_fast_100::Parameters super_fast_100_parameters;
    super_fast_100::Processor super_fast_100_processor;
    std::uint32_t super_fast_100_sample_rate{};
    chainsaw::Parameters chainsaw_parameters;
    chainsaw::Processor chainsaw_processor;
    std::uint32_t chainsaw_sample_rate{};
    skreamer_deluxe::Parameters skreamer_deluxe_parameters;
    skreamer_deluxe::Processor skreamer_deluxe_processor;
    std::uint32_t skreamer_deluxe_sample_rate{};
    kolor::Parameters kolor_parameters;
    kolor::Processor kolor_processor;
    std::uint32_t kolor_sample_rate{};
    reverb_delight::Parameters reverb_delight_parameters;
    reverb_delight::Processor reverb_delight_processor;
    std::uint32_t reverb_delight_sample_rate{};
    hot_solo::Parameters hot_solo_parameters;
    hot_solo::Processor hot_solo_processor;
    std::uint32_t hot_solo_sample_rate{};
    jump::Parameters jump_parameters;
    jump::Processor jump_processor;
    std::uint32_t jump_sample_rate{};
    plate_reverb::Parameters plate_reverb_parameters;
    plate_reverb::Processor plate_reverb_processor;
    std::uint32_t plate_reverb_sample_rate{};
    twang::Parameters twang_parameters;
    twang::Processor twang_processor;
    std::uint32_t twang_sample_rate{};
    ring_modulator::Parameters ring_modulator_parameters;
    ring_modulator::Processor ring_modulator_processor;
    std::uint32_t ring_modulator_sample_rate{};
    dstortion::Parameters dstortion_parameters;
    dstortion::Processor dstortion_processor;
    std::uint32_t dstortion_sample_rate{};
    van51::Parameters van51_parameters;
    van51::Processor van51_processor;
    std::uint32_t van51_sample_rate{};
    bass_invader::Parameters bass_invader_parameters;
    bass_invader::Processor bass_invader_processor;
    std::uint32_t bass_invader_sample_rate{};
    psyche_delay::Parameters psyche_delay_parameters;
    psyche_delay::Processor psyche_delay_processor;
    std::uint32_t psyche_delay_sample_rate{};
    replika_delay::Parameters replika_delay_parameters;
    replika_delay::Processor replika_delay_processor;
    std::uint32_t replika_delay_sample_rate{};
    float limiter_input_gain{};
    float limiter_release_time{ 100.0F };
    legacy_limiter::Processor limiter_processor;
    float transparent_limiter_input{ 1.0F };
    float transparent_limiter_release{ 0.01F };
    float transparent_limiter_ceiling{ 1.0F };
    bool transparent_limiter_external_sidechain{};
    std::uint32_t transparent_limiter_sample_rate{};
    hilbert_limiter::TransparentLimiter transparent_limiter;
    float transient_attack{ 0.5F };
    float transient_sustain{ 0.5F };
    float transient_gain{ 0.5F };
    std::int32_t transient_mode{};
    std::uint32_t transient_sample_rate{};
    transient_master::Processor transient_processor;
    float tape_gain{};
    float tape_warmth{};
    float tape_rolloff{};
    bool tape_high_quality{};
    std::uint32_t tape_sample_rate{};
    bool tape_ready{};
    tape::Processor tape_processor;
    float dyx_morph{};
    float dyx_dry{};
    dyx::ParameterRamp dyx_morph_ramp;
    dyx::ParameterRamp dyx_dry_ramp;
    bool dyx_linear_phase{};
    std::optional<std::size_t> dyx_selected_zone;
    std::vector<dyx::ChannelResponses> dyx_channels;
    std::array<float, 5> dyx_source_bounds{};
    float dyx_coordinate{ 0.5F };
    std::vector<DyxTransferRuntime> dyx_transfers;
    std::array<dyx::StreamingBlockDelay, 2> dyx_source_delay;
    std::uint32_t dyx_sample_rate{};
    std::array<dyx::CubicRateConverter, 2> dyx_ingress;
    std::array<dyx::CubicRateConverter, 2> dyx_egress;
    std::array<std::deque<float>, 2> dyx_output_queue;
    float gainer_target{ 1.0F };
    float gainer_current{ 1.0F };
    std::array<std::optional<float>, 8> send_levels{};
    std::array<routing::SendLevelState, 8> send_level_states{};
    std::array<float, 8> voice_send_modulation{};
    std::array<float, 8> voice_send_previous{};
    std::array<float, 8> voice_send_current{};
    std::array<bool, 8> voice_send_initialized{};
    solid_g_eq::Parameters solid_g_eq_parameters;
    solid_g_eq::Processor solid_g_eq_processor;
    std::uint32_t solid_g_eq_sample_rate{};
    legacy_delay::Parameters legacy_delay_parameters;
    legacy_delay::Processor legacy_delay_processor;
    std::uint32_t legacy_delay_sample_rate{};
    float legacy_delay_tempo_bpm{ -1.F };
    skreamer::Parameters skreamer_parameters;
    skreamer::Processor skreamer_processor;
    std::uint32_t skreamer_sample_rate{};
    cabinet::Parameters cabinet_parameters;
    cabinet::Processor cabinet_processor;
    std::uint32_t cabinet_sample_rate{};
    phaser::Parameters phaser_parameters;
    phaser::Processor phaser_processor;
    std::uint32_t phaser_sample_rate{};
    phasis::Parameters phasis_parameters;
    phasis::Processor phasis_processor;
    std::uint32_t phasis_sample_rate{};
    float phasis_tempo_bpm{};
    legacy_reverb::Parameters legacy_reverb_parameters;
    legacy_reverb::Processor legacy_reverb_processor;
    std::uint32_t legacy_reverb_sample_rate{};
    galois_reverb::Parameters galois_reverb_parameters;
    galois_reverb::Processor galois_reverb_processor;
    std::uint32_t galois_reverb_sample_rate{};
    chorus::Parameters chorus_parameters;
    chorus::Processor chorus_processor;
    std::uint32_t chorus_sample_rate{};
    chorus::ChoralParameters choral_parameters;
    chorus::ChoralProcessor choral_processor;
    std::uint32_t choral_sample_rate{};
    chorus::FlangerParameters flanger_parameters;
    chorus::FlangerProcessor flanger_processor;
    std::uint32_t flanger_sample_rate{};
    rotator::Parameters rotator_parameters;
    rotator::Processor rotator_processor;
    std::uint32_t rotator_sample_rate{};
    routing::SurroundParameters surround_parameters;
    routing::SurroundProcessor surround_processor;
    std::uint32_t surround_sample_rate{};
    float cutoff{ 1.0F };
    float resonance{};
    float last_cutoff{ -1.0F };
    float last_resonance{ -1.0F };
    std::uint32_t last_sample_rate{};
    float output_gain{ 1.0F };
    float phaser_coefficient{};
    float phaser_feedback{};
    float pro52_first_step{};
    float pro52_second_step{};
    float pro52_first_damping{};
    float pro52_second_damping{};
    float ladder_step{};
    float ladder_resonance{};
    current_ladder::Filter current_ladder_filter;
    filter_daft::Filter daft_filter;
    filter_dj::Filter dj_filter;
    formant::Filter formant_filter;
    multi_state_variable::Filter multi_state_variable_filter;
    state_variable_1::Filter state_variable_1_filter;
    state_variable_4::Filter state_variable_4_filter;
    state_variable_6::Filter state_variable_6_filter;
    blue_monark::Filter blue_monark_filter;
    simple_lphp::Filter simple_lphp_filter;
    state_variable_2::Filter state_variable_2_filter;
    parametric_eq::Filter parametric_eq_filter;
    float simple_lphp_low_pass{ 1.0F };
    float simple_lphp_high_pass{};
    std::uint32_t simple_lphp_sample_rate{};
    bool simple_lphp_ready{};
    bool state_variable_2_ready{};
    std::uint32_t parametric_eq_sample_rate{};
    bool parametric_eq_ready{};
    std::uint32_t current_ladder_serialized_type{};
    float current_ladder_input_gain{};
    float daft_input_gain{};
    bool current_ladder_high_quality{};
    std::uint32_t current_ladder_sample_rate{};
    bool current_ladder_ready{};
    bool daft_ready{};
    std::uint32_t dj_sample_rate{};
    bool dj_ready{};
    bool formant_ready{};
    float formant_position{};
    float formant_shift{};
    float formant_sharpness{};
    float multi_bandwidth{};
    bool multi_state_variable_ready{};
    bool state_variable_1_ready{};
    bool state_variable_4_ready{};
    bool state_variable_6_ready{};
    bool blue_monark_ready{};
    std::optional<float> versatile_gain;
    std::array<std::optional<float>, 3> versatile_shift{};
    std::array<std::optional<float>, 3> versatile_resonance{};
    std::array<std::optional<float>, 3> versatile_type{};
    std::array<std::optional<float>, 3> versatile_bypass{};
    std::array<float, 3> versatile_step{};
    std::array<float, 3> versatile_damping{};
    std::array<float, 3> versatile_mix{};
    std::array<std::array<float, 3>, 3> versatile_weights{};
    float versatile_gain_target{};
    float versatile_gain_smoothing{};
    std::array<std::optional<float>, 3> eq_frequency{};
    std::array<std::optional<float>, 3> eq_bandwidth{};
    std::array<std::optional<float>, 3> eq_gain{};
    std::array<std::array<float, 3>, 3> voice_eq_values{};
    std::array<bool, 3> voice_eq_modulated{};
    std::array<unsigned, 3> voice_eq_component_masks{};
    std::array<float, 5> coefficients{}; // x1, x2, y1, y2, input
    std::array<float, 5> parallel_coefficients{};
    std::array<float, 5> third_coefficients{};
    std::array<double, 5> double_coefficients{};
    std::array<double, 5> parallel_double_coefficients{};
    std::array<float, 2> x1{};
    std::array<float, 2> x2{};
    std::array<float, 2> y1{};
    std::array<float, 2> y2{};
    std::array<float, 2> parallel_x1{};
    std::array<float, 2> parallel_x2{};
    std::array<float, 2> parallel_y1{};
    std::array<float, 2> parallel_y2{};
    std::array<float, 2> third_x1{};
    std::array<float, 2> third_x2{};
    std::array<float, 2> third_y1{};
    std::array<float, 2> third_y2{};
    std::array<std::array<float, 4>, 2> phaser_x{};
    std::array<std::array<float, 4>, 2> phaser_y{};
    std::array<std::array<float, 5>, 2> pro52_state{};
    std::array<std::array<float, 6>, 2> ladder_state{};
    std::array<std::array<float, 7>, 2> versatile_state{};
    std::array<bool, 2> versatile_initialized{};

    static void process_legacy_limiter(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        effect.limiter_processor.configure(sample_rate,
                effect.limiter_input_gain, effect.limiter_release_time);
        const auto limited = effect.limiter_processor.process(left, right);
        left = limited.first;
        right = limited.second;
    }

    static void process_inverter(EffectRuntime &effect, std::uint32_t, float,
            float &left, float &right) {
        if (effect.inverter_panorama)
            std::swap(left, right);
        if (effect.inverter_phase) {
            left = -left;
            right = -right;
        }
    }

    static void process_tape(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.tape_sample_rate != sample_rate) {
            effect.tape_ready = effect.tape_processor.configure(sample_rate,
                    effect.tape_gain, effect.tape_warmth, effect.tape_rolloff,
                    effect.tape_high_quality);
            effect.tape_sample_rate = sample_rate;
        }
        if (!effect.tape_ready)
            return;
        const auto saturated = effect.tape_processor.process(left, right);
        left = saturated.first;
        right = saturated.second;
    }

    static void process_replika_delay(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.replika_delay_sample_rate != sample_rate) {
            effect.replika_delay_processor.configure(
                    sample_rate, effect.replika_delay_parameters);
            effect.replika_delay_sample_rate = sample_rate;
        }
        const auto delayed =
                effect.replika_delay_processor.process(left, right);
        left = delayed.first;
        right = delayed.second;
    }

    static void process_irc(EffectRuntime &effect, std::uint32_t, float,
            float &left, float &right) {
        const auto wet = effect.irc_convolver.process(left, right);
        left = wet.first;
        right = wet.second;
    }

    static void process_solid_g_eq(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.solid_g_eq_sample_rate != sample_rate) {
            effect.solid_g_eq_processor.configure(
                    sample_rate, effect.solid_g_eq_parameters);
            effect.solid_g_eq_sample_rate = sample_rate;
        }
        const auto filtered = effect.solid_g_eq_processor.process(left, right);
        left = filtered.first;
        right = filtered.second;
    }

    static void process_legacy_delay(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo, float &left, float &right) {
        if (effect.legacy_delay_sample_rate != sample_rate ||
                effect.legacy_delay_tempo_bpm != tempo) {
            effect.legacy_delay_processor.configure(
                    sample_rate, effect.legacy_delay_parameters, tempo);
            effect.legacy_delay_sample_rate = sample_rate;
            effect.legacy_delay_tempo_bpm = tempo;
        }
        const auto delayed = effect.legacy_delay_processor.process(left, right);
        left = delayed.first;
        right = delayed.second;
    }

    static void process_skreamer(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.skreamer_sample_rate != sample_rate) {
            effect.skreamer_processor.configure(
                    sample_rate, effect.skreamer_parameters);
            effect.skreamer_sample_rate = sample_rate;
        }
        effect.skreamer_processor.process(left, right);
    }

    static void process_cabinet(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.cabinet_sample_rate != sample_rate) {
            effect.cabinet_processor.configure(
                    sample_rate, effect.cabinet_parameters);
            effect.cabinet_sample_rate = sample_rate;
        }
        effect.cabinet_processor.process(left, right);
    }

    static void process_legacy_phaser(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.phaser_sample_rate != sample_rate) {
            effect.phaser_processor.configure(
                    sample_rate, effect.phaser_parameters);
            effect.phaser_sample_rate = sample_rate;
        }
        const auto phased =
                effect.phaser_processor.process(left, right, tempo_bpm);
        left = phased.first;
        right = phased.second;
    }

    static void process_phasis(EffectRuntime &effect, std::uint32_t sample_rate,
            float tempo_bpm, float &left, float &right) {
        if (effect.phasis_sample_rate != sample_rate ||
                effect.phasis_tempo_bpm != tempo_bpm) {
            effect.phasis_parameters.tempo_bpm = tempo_bpm;
            effect.phasis_processor.configure(
                    sample_rate, effect.phasis_parameters);
            effect.phasis_sample_rate = sample_rate;
            effect.phasis_tempo_bpm = tempo_bpm;
        }
        const auto phased = effect.phasis_processor.process(left, right);
        left = phased.first;
        right = phased.second;
    }

    static void process_flair(EffectRuntime &effect, std::uint32_t sample_rate,
            float tempo_bpm, float &left, float &right) {
        if (effect.flair_sample_rate != sample_rate ||
                effect.flair_tempo_bpm != tempo_bpm) {
            effect.flair_parameters.tempo_bpm = tempo_bpm;
            effect.flair_processor.configure(
                    sample_rate, effect.flair_parameters);
            effect.flair_sample_rate = sample_rate;
            effect.flair_tempo_bpm = tempo_bpm;
        }
        const auto flanged = effect.flair_processor.process(left, right);
        left = flanged.first;
        right = flanged.second;
    }

    static void process_cry_wah(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.cry_wah_sample_rate != sample_rate) {
            effect.cry_wah_processor.configure(
                    sample_rate, effect.cry_wah_parameters);
            effect.cry_wah_sample_rate = sample_rate;
        }
        const auto value = effect.cry_wah_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_cat(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.cat_sample_rate != sample_rate) {
            effect.cat_processor.configure(sample_rate, effect.cat_parameters);
            effect.cat_sample_rate = sample_rate;
        }
        const auto value = effect.cat_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_ac_box(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.ac_box_sample_rate != sample_rate) {
            effect.ac_box_processor.configure(
                    sample_rate, effect.ac_box_parameters);
            effect.ac_box_sample_rate = sample_rate;
        }
        const auto value = effect.ac_box_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_bass_pro(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.bass_pro_sample_rate != sample_rate) {
            effect.bass_pro_processor.configure(
                    sample_rate, effect.bass_pro_parameters);
            effect.bass_pro_sample_rate = sample_rate;
        }
        const auto value = effect.bass_pro_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_twin_delay(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.twin_delay_sample_rate != sample_rate ||
                effect.twin_delay_tempo_bpm != tempo_bpm) {
            effect.twin_delay_parameters.tempo_bpm = tempo_bpm;
            effect.twin_delay_processor.configure(
                    sample_rate, effect.twin_delay_parameters);
            effect.twin_delay_sample_rate = sample_rate;
            effect.twin_delay_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.twin_delay_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_raum(EffectRuntime &effect, std::uint32_t sample_rate,
            float tempo_bpm, float &left, float &right) {
        if (effect.raum_sample_rate != sample_rate ||
                effect.raum_tempo_bpm != tempo_bpm) {
            effect.raum_parameters.tempo_bpm = tempo_bpm;
            effect.raum_processor.configure(
                    sample_rate, effect.raum_parameters);
            effect.raum_sample_rate = sample_rate;
            effect.raum_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.raum_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_bite(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.bite_sample_rate != sample_rate) {
            effect.bite_processor.configure(
                    sample_rate, effect.bite_parameters);
            effect.bite_sample_rate = sample_rate;
        }
        const auto value = effect.bite_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_dirt(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.dirt_sample_rate != sample_rate) {
            effect.dirt_processor.configure(
                    sample_rate, effect.dirt_parameters);
            effect.dirt_sample_rate = sample_rate;
        }
        const auto value = effect.dirt_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_freak(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.freak_sample_rate != sample_rate) {
            effect.freak_processor.configure(
                    sample_rate, effect.freak_parameters);
            effect.freak_sample_rate = sample_rate;
        }
        const auto value = effect.freak_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_vibrato_chorus(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.vibrato_chorus_sample_rate != sample_rate) {
            effect.vibrato_chorus_processor.configure(
                    sample_rate, effect.vibrato_chorus_parameters);
            effect.vibrato_chorus_sample_rate = sample_rate;
        }
        const auto value = effect.vibrato_chorus_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_wow_flutter(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.wow_flutter_sample_rate != sample_rate) {
            effect.wow_flutter_processor.configure(
                    sample_rate, effect.wow_flutter_parameters);
            effect.wow_flutter_sample_rate = sample_rate;
        }
        const auto value = effect.wow_flutter_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_ep_preamps(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.ep_preamps_sample_rate != sample_rate) {
            effect.ep_preamps_processor.configure(
                    sample_rate, effect.ep_preamps_parameters);
            effect.ep_preamps_sample_rate = sample_rate;
        }
        const auto value = effect.ep_preamps_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_fuzz(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.fuzz_sample_rate != sample_rate) {
            effect.fuzz_processor.configure(
                    sample_rate, effect.fuzz_parameters);
            effect.fuzz_sample_rate = sample_rate;
        }
        const auto value = effect.fuzz_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_big_fuzz(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.big_fuzz_sample_rate != sample_rate) {
            effect.big_fuzz_processor.configure(
                    sample_rate, effect.big_fuzz_parameters);
            effect.big_fuzz_sample_rate = sample_rate;
        }
        const auto value = effect.big_fuzz_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_stereo_tune(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.stereo_tune_sample_rate != sample_rate) {
            effect.stereo_tune_processor.configure(
                    sample_rate, effect.stereo_tune_parameters);
            effect.stereo_tune_sample_rate = sample_rate;
        }
        const auto value = effect.stereo_tune_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_beat_masher(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.beat_masher_sample_rate != sample_rate ||
                effect.beat_masher_tempo_bpm != tempo_bpm) {
            effect.beat_masher_processor.configure(
                    sample_rate, tempo_bpm, effect.beat_masher_parameters);
            effect.beat_masher_sample_rate = sample_rate;
            effect.beat_masher_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.beat_masher_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_beat_slicer(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.beat_slicer_sample_rate != sample_rate ||
                effect.beat_slicer_tempo_bpm != tempo_bpm) {
            effect.beat_slicer_processor.configure(
                    sample_rate, tempo_bpm, effect.beat_slicer_parameters);
            effect.beat_slicer_sample_rate = sample_rate;
            effect.beat_slicer_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.beat_slicer_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_gater(EffectRuntime &effect, std::uint32_t sample_rate,
            float tempo_bpm, float &left, float &right) {
        if (effect.gater_sample_rate != sample_rate ||
                effect.gater_tempo_bpm != tempo_bpm) {
            effect.gater_processor.configure(
                    sample_rate, tempo_bpm, effect.gater_parameters);
            effect.gater_sample_rate = sample_rate;
            effect.gater_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.gater_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_reverse_grain(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.reverse_grain_sample_rate != sample_rate ||
                effect.reverse_grain_tempo_bpm != tempo_bpm) {
            effect.reverse_grain_processor.configure(
                    sample_rate, tempo_bpm, effect.reverse_grain_parameters);
            effect.reverse_grain_sample_rate = sample_rate;
            effect.reverse_grain_tempo_bpm = tempo_bpm;
        }
        const auto value = effect.reverse_grain_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_transpose_stretch(EffectRuntime &effect,
            std::uint32_t sample_rate, float tempo_bpm, float &left,
            float &right) {
        if (effect.transpose_stretch_sample_rate != sample_rate ||
                effect.transpose_stretch_tempo_bpm != tempo_bpm) {
            effect.transpose_stretch_processor.configure(sample_rate, tempo_bpm,
                    effect.transpose_stretch_parameters);
            effect.transpose_stretch_sample_rate = sample_rate;
            effect.transpose_stretch_tempo_bpm = tempo_bpm;
        }
        const auto value =
                effect.transpose_stretch_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    template <auto ProcessorMember, auto ParametersMember,
            auto SampleRateMember>
    static void process_configured_stereo(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        auto &rate = effect.*SampleRateMember;
        auto &processor = effect.*ProcessorMember;
        if (rate != sample_rate) {
            processor.configure(sample_rate, effect.*ParametersMember);
            rate = sample_rate;
        }
        const auto value = processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void begin_effect_span(EffectRuntime &effect,
            std::uint32_t sample_rate, std::size_t frames) {
        if (effect.effect_type == libnki::GroupEffectType::lo_fi) {
            // the whole span so all the left noise gets drawn before the right
            (void)begin_lo_fi_span(effect, frames, {}, {});
            return;
        }
        if (effect.effect_type == libnki::GroupEffectType::flanger) {
            if (effect.flanger_sample_rate != sample_rate) {
                effect.flanger_processor.configure(
                        sample_rate, effect.flanger_parameters);
                effect.flanger_sample_rate = sample_rate;
            }
            effect.flanger_processor.begin_span(frames);
            return;
        }
        if (effect.effect_type == libnki::GroupEffectType::d_stortion) {
            if (effect.dstortion_sample_rate != sample_rate) {
                effect.dstortion_processor.configure(
                        sample_rate, effect.dstortion_parameters);
                effect.dstortion_sample_rate = sample_rate;
            }
            // once per span, not per sample, or the 32 frame timing moves
            effect.dstortion_processor.begin_span(frames);
            return;
        }
        if (effect.effect_type == libnki::GroupEffectType::cabinet) {
            if (effect.cabinet_sample_rate != sample_rate) {
                effect.cabinet_processor.configure(
                        sample_rate, effect.cabinet_parameters);
                effect.cabinet_sample_rate = sample_rate;
            }
            // once per span
            effect.cabinet_processor.begin_span(frames);
            return;
        }
        if (effect.effect_type == libnki::GroupEffectType::hot_solo) {
            if (effect.hot_solo_sample_rate != sample_rate) {
                effect.hot_solo_processor.configure(
                        sample_rate, effect.hot_solo_parameters);
                effect.hot_solo_sample_rate = sample_rate;
            }
            // once per span, same as above
            effect.hot_solo_processor.begin_span(frames);
            return;
        }
        if (effect.effect_type != libnki::GroupEffectType::rotator)
            return;
        if (effect.rotator_sample_rate != sample_rate) {
            effect.rotator_processor.configure(
                    sample_rate, effect.rotator_parameters);
            effect.rotator_sample_rate = sample_rate;
        }
        effect.rotator_processor.begin_span(frames);
    }

    static void process_compressor(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        std::pair<float, float> value;
        if (effect.compressor_model == libnki::CompressorModel::pro) {
            effect.compressor_pro.configure(sample_rate,
                    effect.compressor_threshold, effect.compressor_ratio,
                    effect.compressor_attack, effect.compressor_release);
            value = effect.compressor_pro.process(left, right);
        } else if (effect.compressor_model ==
                   libnki::CompressorModel::enhanced) {
            effect.compressor_enhanced.configure(sample_rate,
                    effect.compressor_threshold, effect.compressor_ratio,
                    effect.compressor_attack, effect.compressor_release,
                    effect.compressor_stereo_link);
            value = effect.compressor_enhanced.process(left, right);
        } else {
            effect.compressor_classic.configure(sample_rate,
                    effect.compressor_threshold, effect.compressor_ratio,
                    effect.compressor_attack, effect.compressor_release,
                    effect.compressor_stereo_link);
            value = effect.compressor_classic.process(left, right);
        }
        left = value.first;
        right = value.second;
    }

    // the arrays have to live until the whole span is done. once per span, NOT
    // per sample
    static bool begin_lo_fi_span(EffectRuntime &effect, std::size_t frames,
            std::span<const float> bitdepth,
            std::span<const float> downsample) {
        effect.lo_fi_bitdepth_plane = {};
        effect.lo_fi_downsample_plane = {};
        effect.lo_fi_span_remaining = 0;
        effect.lo_fi_chunk_remaining = 0;
        effect.lo_fi_noise_chunk_remaining = 0;
        effect.lo_fi_next_control_index = 0;
        if (effect.lo_fi_control_countdown > 32)
            return false;
        const auto residual = effect.lo_fi_control_countdown;
        const std::size_t needed = frames == 0 || frames < residual
                                           ? 0
                                           : 1 + (frames - residual) / 32;
        const auto valid = [needed](std::span<const float> plane) {
            return plane.empty() ||
                   (plane.size() >= needed &&
                           std::ranges::all_of(plane.first(needed),
                                   [](float x) { return std::isfinite(x); }));
        };
        if (!valid(bitdepth) || !valid(downsample))
            return false;
        effect.lo_fi_bitdepth_plane = bitdepth;
        effect.lo_fi_downsample_plane = downsample;
        effect.lo_fi_span_remaining = frames;
        return true;
    }

    static void refresh_lo_fi_bits(EffectRuntime &effect, float control) {
        effect.lo_fi_bits_active = control < 0.99999F;
        if (effect.lo_fi_bits_active) {
            // exp2 in double, exp2f rounds differently
            const float exponent = std::max(control * 16.0F, 1.0F);
            effect.lo_fi_quantize_scale = static_cast<float>(
                    std::exp2(static_cast<double>(exponent)));
            effect.lo_fi_quantize_inverse = 1.0F / effect.lo_fi_quantize_scale;
        }
    }

    static void refresh_lo_fi_downsample(EffectRuntime &effect,
            std::uint32_t sample_rate, float control, bool reset_phase) {
        effect.lo_fi_down_sample_active = control < 0.99999F;
        if (effect.lo_fi_down_sample_active) {
            const float rate = static_cast<float>(sample_rate);
            const float frequency = std::max(control * rate, 50.0F);
            effect.lo_fi_hold_increment = std::max(rate / frequency, 1.0F);
            if (effect.lo_fi_sample_hold_integer)
                effect.lo_fi_hold_increment = static_cast<float>(
                        static_cast<int>(effect.lo_fi_hold_increment));
            // reset only when refreshing an enabled downsampler
            if (reset_phase)
                effect.lo_fi_hold_phase = effect.lo_fi_hold_increment;
        }
    }

    static std::uint32_t advance_lo_fi_rng(
            std::uint32_t state, std::size_t count) {
        // skip ahead in O(log n) to where the right channel starts
        std::uint32_t multiply = 0x0bb38435U, add = 0x3619636bU;
        while (count) {
            if (count & 1U)
                state = state * multiply + add;
            add *= multiply + 1U;
            multiply *= multiply;
            count >>= 1U;
        }
        return state;
    }

    static void process_lo_fi(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.lo_fi_sample_rate != sample_rate) {
            // float here, only the dynamic one above is double
            effect.lo_fi_bits_active = effect.lo_fi_bits < 0.99999F;
            if (effect.lo_fi_bits_active) {
                const float bits = std::max(effect.lo_fi_bits * 16.0F, 1.0F);
                effect.lo_fi_quantize_scale = std::exp2(bits);
                effect.lo_fi_quantize_inverse =
                        1.0F / effect.lo_fi_quantize_scale;
            }
            float down_sample = effect.lo_fi_down_sample;
            if (effect.lo_fi_down_sample_requires_engine_rate_conversion)
                down_sample *= 16000.0F / static_cast<float>(sample_rate);
            refresh_lo_fi_downsample(effect, sample_rate, down_sample, false);
            effect.lo_fi_sample_rate = sample_rate;
        }
        // refresh BEFORE the chunk, per sample moves it to the end
        while (effect.lo_fi_chunk_remaining == 0 &&
                effect.lo_fi_span_remaining != 0) {
            auto chunk = effect.lo_fi_span_remaining;
            if (!effect.lo_fi_bitdepth_plane.empty() ||
                    !effect.lo_fi_downsample_plane.empty()) {
                chunk = std::min(
                        chunk, static_cast<std::size_t>(
                                       effect.lo_fi_control_countdown));
                effect.lo_fi_control_countdown -=
                        static_cast<std::uint32_t>(chunk);
                if (effect.lo_fi_control_countdown == 0) {
                    effect.lo_fi_control_countdown = 32;
                    const auto index = effect.lo_fi_next_control_index++;
                    if (!effect.lo_fi_bitdepth_plane.empty())
                        refresh_lo_fi_bits(
                                effect, effect.lo_fi_bitdepth_plane[index]);
                    if (!effect.lo_fi_downsample_plane.empty())
                        refresh_lo_fi_downsample(effect, sample_rate,
                                effect.lo_fi_downsample_plane[index], true);
                }
            }
            effect.lo_fi_span_remaining -= chunk;
            effect.lo_fi_chunk_remaining = chunk;
        }
        if (effect.lo_fi_noise_level >= 0.001F &&
                effect.lo_fi_noise_chunk_remaining == 0) {
            const auto chunk =
                    std::max<std::size_t>(effect.lo_fi_chunk_remaining, 1);
            effect.lo_fi_noise_channel_rng = { effect.lo_fi_random_state,
                advance_lo_fi_rng(effect.lo_fi_random_state, chunk) };
            effect.lo_fi_noise_chunk_remaining = chunk;
        }
        if (effect.lo_fi_chunk_remaining != 0)
            --effect.lo_fi_chunk_remaining;
        if (effect.lo_fi_down_sample_active) {
            if (effect.lo_fi_hold_phase >= effect.lo_fi_hold_increment) {
                effect.lo_fi_hold_phase -= effect.lo_fi_hold_increment;
                effect.lo_fi_held = { left, right };
            } else {
                effect.lo_fi_hold_phase += 1.0F;
                left = effect.lo_fi_held[0];
                right = effect.lo_fi_held[1];
            }
        }
        if (effect.lo_fi_bits_active) {
            const auto quantize = [&](float value) {
                return effect.lo_fi_quantize_inverse *
                       static_cast<float>(static_cast<int>(
                               value * effect.lo_fi_quantize_scale));
            };
            left = quantize(left);
            right = quantize(right);
        }
        if (effect.lo_fi_noise_level >= 0.001F) {
            const float color =
                    1.0F -
                    std::clamp(effect.lo_fi_noise_color, 0.0F, 1.0F) * 0.9F;
            // double for the .075, narrow once, both use fma
            const float amount = static_cast<float>(
                    static_cast<double>(effect.lo_fi_noise_level) * .075);
            const auto add_noise = [&](float value, std::size_t channel) {
                auto &rng = effect.lo_fi_noise_channel_rng[channel];
                rng = rng * 0x0bb38435U + 0x3619636bU;
                const float random = std::bit_cast<float>((rng & 0x007fffffU) |
                                                          0x40000000U) -
                                     3.0F;
                auto &state = effect.lo_fi_noise_state[channel];
                state = std::fma(random - state, color, state);
                return std::fma(state, amount, value);
            };
            left = add_noise(left, 0);
            right = add_noise(right, 1);
            effect.lo_fi_random_state = effect.lo_fi_noise_channel_rng[1];
            --effect.lo_fi_noise_chunk_remaining;
        }
    }

    static void process_shaper(EffectRuntime &effect, std::uint32_t sample_rate,
            float, float &left, float &right) {
        if (effect.shaper_model == libnki::ShaperModel::enhanced) {
            const float position = (effect.shaper_shape + 1.0F) * 0.5F * 127.0F;
            const std::size_t table_index = std::min<std::size_t>(
                    static_cast<std::size_t>(position), 126);
            const float fraction =
                    position >= 127.0F
                            ? 1.0F
                            : position - static_cast<float>(table_index);
            const auto lookup = [&](const auto &bits) {
                const float first = std::bit_cast<float>(bits[table_index]);
                const float second =
                        std::bit_cast<float>(bits[table_index + 1]);
                return first + (second - first) * fraction;
            };
            const float drive = lookup(detail::drive_amount_bits);
            const float mix = lookup(detail::drive_mix_bits);
            const float gain = lookup(detail::drive_gain_correction_bits);
            const auto shape = [&](float value) {
                const float driven = std::clamp(value * drive, -8.0F, 8.0F);
                const float curved =
                        gain * driven * (std::abs(driven) * -0.0625F + 1.0F);
                return value + (curved - value) * mix;
            };
            left = shape(left);
            right = shape(right);
            return;
        }
        if (effect.shaper_model == libnki::ShaperModel::drums) {
            if (effect.shaper_shape <= -0.9999F)
                return;
            if (effect.shaper_sample_rate != sample_rate) {
                const auto coefficient = [sample_rate](float milliseconds) {
                    // clang-format off
                    const float samples =
                            std::max(static_cast<float>(sample_rate) * 0.001F *
                            milliseconds - 1.0F, 1.0F);
                    // clang-format on
                    return 1.0F - std::exp2(-6.643856F / samples);
                };
                effect.shaper_detector_release = coefficient(25.4764F);
                effect.shaper_gain_smoothing = coefficient(2.67874F);
                effect.shaper_sample_rate = sample_rate;
            }
            const float peak = std::max(std::abs(left), std::abs(right));
            if (peak >= effect.shaper_detector)
                effect.shaper_detector = peak;
            else
                effect.shaper_detector += effect.shaper_detector_release *
                                          (peak - effect.shaper_detector);
            const float level =
                    std::min(effect.shaper_detector + 1.0e-5F, 1.0F);
            const float level_position = level * 127.0F;
            const std::size_t level_index = std::min<std::size_t>(
                    static_cast<std::size_t>(level_position), 126);
            const float level_fraction =
                    level_position >= 127.0F
                            ? 1.0F
                            : level_position - static_cast<float>(level_index);
            const float shape_position = (effect.shaper_shape + 1.0F) * 25.0F;
            const std::size_t shape_index = std::min<std::size_t>(
                    static_cast<std::size_t>(shape_position), 49);
            const float shape_fraction =
                    shape_position >= 50.0F
                            ? 1.0F
                            : shape_position - static_cast<float>(shape_index);
            const auto curve = [&](std::size_t row) {
                const std::size_t base = row * 128;
                const float first = std::bit_cast<float>(
                        detail::compress_curve_bits[base + level_index]);
                const float second = std::bit_cast<float>(
                        detail::compress_curve_bits[base + level_index + 1]);
                return first + (second - first) * level_fraction;
            };
            const float first_curve = curve(shape_index);
            const float compressed =
                    first_curve +
                    (curve(shape_index + 1) - first_curve) * shape_fraction;
            const float target_gain = compressed / level;
            effect.shaper_gain += effect.shaper_gain_smoothing *
                                  (target_gain - effect.shaper_gain);
            const float correction_first = std::bit_cast<float>(
                    detail::compress_gain_correction_bits[shape_index]);
            const float correction_second = std::bit_cast<float>(
                    detail::compress_gain_correction_bits[shape_index + 1]);
            const float correction =
                    correction_first +
                    (correction_second - correction_first) * shape_fraction;
            const float gain = effect.shaper_gain * correction;
            left *= gain;
            right *= gain;
            return;
        }
        const float drive = (effect.shaper_shape + 1.0F) * 4.0F;
        if (drive > 3.999F && drive < 4.0001F)
            return;
        const auto soft_clip = [](float value) {
            const float magnitude = std::clamp(std::abs(value), 0.0F, 1.0F);
            return std::copysign(
                    magnitude * 2.0F - magnitude * magnitude, value);
        };
        const auto shape = [&](float value) {
            if (drive >= 5.0F)
                return soft_clip(value * (drive - 4.0F));
            if (drive >= 4.0F) {
                const float mix = drive - 4.0F;
                return mix * soft_clip(value) + (1.0F - mix) * value;
            }
            const float amount = drive * 0.975F + 0.1F;
            return (amount * value + value * value * value) / (amount + 1.0F);
        };
        left = shape(left);
        right = shape(right);
    }

    static void process_transparent_limiter(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.transparent_limiter_sample_rate != sample_rate) {
            effect.transparent_limiter.configure(sample_rate,
                    effect.transparent_limiter_input,
                    effect.transparent_limiter_release,
                    effect.transparent_limiter_ceiling,
                    effect.transparent_limiter_external_sidechain);
            effect.transparent_limiter_sample_rate = sample_rate;
        }
        const auto value = effect.transparent_limiter.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_transient_master(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        if (effect.transient_sample_rate != sample_rate) {
            effect.transient_processor.configure(sample_rate,
                    effect.transient_attack, effect.transient_sustain,
                    effect.transient_gain, effect.transient_mode);
            effect.transient_sample_rate = sample_rate;
        }
        const auto value = effect.transient_processor.process(left, right);
        left = value.first;
        right = value.second;
    }

    static void process_voice_stereo_spread(EffectRuntime &effect,
            std::uint32_t sample_rate, float &left, float &right) {
        const float source_left = left;
        const float source_right = right;
        const bool bypass_core = !effect.stereo_pseudo &&
                                 effect.stereo_spread_target > 0.499F &&
                                 effect.stereo_spread_target < 0.5001F &&
                                 effect.stereo_pan_target > -0.001F &&
                                 effect.stereo_pan_target < 0.001F;
        if (bypass_core) {
            // pass through here
        } else if (effect.stereo_pseudo) {
            const auto delay = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(static_cast<float>(sample_rate) *
                                               effect.stereo_spread_target *
                                               effect.stereo_spread_target *
                                               effect.stereo_spread_target *
                                               0.01F),
                    1023U);
            effect.stereo_delay[(effect.stereo_delay_index + delay) & 1023U] =
                    source_right;
            left = source_left;
            right = effect.stereo_delay[effect.stereo_delay_index];
            effect.stereo_delay_index =
                    (effect.stereo_delay_index + 1U) & 1023U;
        } else if (effect.stereo_spread_current >= 0.5F) {
            const float side = effect.stereo_spread_current * 2.0F - 1.0F;
            left = source_left + side * source_left - side * source_right;
            right = source_right + side * source_right - side * source_left;
        } else {
            const float cross = 0.5F - effect.stereo_spread_current;
            const float own = 1.0F - cross;
            left = cross * source_right + own * source_left;
            right = cross * source_left + own * source_right;
        }
        if (bypass_core)
            return;
        if (std::abs(effect.stereo_spread_current -
                     effect.stereo_spread_target) >= 1.0e-15F) {
            effect.stereo_spread_current +=
                    (effect.stereo_spread_target -
                            effect.stereo_spread_current) *
                    0.0055555557F;
            if (std::abs(effect.stereo_spread_current -
                         effect.stereo_spread_target) < 1.0e-10F)
                effect.stereo_spread_current = effect.stereo_spread_target;
        }
        const float pan = effect.stereo_pan_current;
        left *= 1.0F - std::clamp(pan, 0.0F, 1.0F);
        right *= std::clamp(pan + 1.0F, 0.0F, 1.0F);
        if (std::abs(effect.stereo_pan_current - effect.stereo_pan_target) >=
                1.0e-15F) {
            effect.stereo_pan_current +=
                    (effect.stereo_pan_target - effect.stereo_pan_current) *
                    0.00055555557F;
            if (std::abs(effect.stereo_pan_current - effect.stereo_pan_target) <
                    1.0e-10F)
                effect.stereo_pan_current = effect.stereo_pan_target;
        }
    }

    static void process_program_stereo_spread(EffectRuntime &effect,
            std::uint32_t sample_rate, float &left, float &right) {
        const float source_left = left;
        const float source_right = right;
        const bool bypass_core = !effect.stereo_pseudo &&
                                 effect.stereo_spread_target > 0.499F &&
                                 effect.stereo_spread_target < 0.5001F &&
                                 effect.stereo_pan_target > -0.001F &&
                                 effect.stereo_pan_target < 0.001F;
        if (!bypass_core && effect.stereo_pseudo) {
            const auto delay = std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(static_cast<float>(sample_rate) *
                                               effect.stereo_spread_target *
                                               effect.stereo_spread_target *
                                               effect.stereo_spread_target *
                                               0.01F),
                    1023U);
            effect.stereo_delay[(effect.stereo_delay_index + delay) & 1023U] =
                    source_right;
            left = source_left;
            right = effect.stereo_delay[effect.stereo_delay_index];
            effect.stereo_delay_index =
                    (effect.stereo_delay_index + 1U) & 1023U;
        } else if (!bypass_core && effect.stereo_spread_current >= 0.5F) {
            const float side = effect.stereo_spread_current * 2.0F - 1.0F;
            left = source_left + side * (source_left - source_right);
            right = source_right + side * (source_right - source_left);
        } else if (!bypass_core) {
            const float cross = 0.5F - effect.stereo_spread_current;
            left = cross * source_right + (1.0F - cross) * source_left;
            right = cross * source_left + (1.0F - cross) * source_right;
        }
        if (!bypass_core) {
            const float pan = effect.stereo_pan_current;
            left *= 1.0F - std::clamp(pan, 0.0F, 1.0F);
            right *= std::clamp(pan + 1.0F, 0.0F, 1.0F);
        }
    }

    static void process_distortion(EffectRuntime &effect,
            std::uint32_t sample_rate, float, float &left, float &right) {
        effect.distortion_processor.configure(sample_rate,
                effect.distortion_model == libnki::DistortionModel::transistor,
                effect.distortion_drive, effect.distortion_damping);
        const auto distorted = effect.distortion_processor.process(left, right);
        left = distorted.first;
        right = distorted.second;
    }

    [[nodiscard]] static SampleProcessor resolve_sample_processor(
            libnki::GroupEffectType type) {
        switch (type) {
            case libnki::GroupEffectType::distortion:
                return &process_distortion;
            case libnki::GroupEffectType::limiter:
                return &process_legacy_limiter;
            case libnki::GroupEffectType::inverter:
                return &process_inverter;
            case libnki::GroupEffectType::tape:
                return &process_tape;
            case libnki::GroupEffectType::replika_delay:
                return &process_replika_delay;
            case libnki::GroupEffectType::solid_g_eq:
                return &process_solid_g_eq;
            case libnki::GroupEffectType::delay:
                return &process_legacy_delay;
            case libnki::GroupEffectType::skreamer:
                return &process_skreamer;
            case libnki::GroupEffectType::cabinet:
                return &process_cabinet;
            case libnki::GroupEffectType::legacy_phaser:
                return &process_legacy_phaser;
            case libnki::GroupEffectType::phasis:
                return &process_phasis;
            case libnki::GroupEffectType::flair:
                return &process_flair;
            case libnki::GroupEffectType::cry_wah:
                return &process_cry_wah;
            case libnki::GroupEffectType::cat:
                return &process_cat;
            case libnki::GroupEffectType::ac_box:
                return &process_ac_box;
            case libnki::GroupEffectType::bass_pro:
                return &process_bass_pro;
            case libnki::GroupEffectType::twin_delay:
                return &process_twin_delay;
            case libnki::GroupEffectType::raum:
                return &process_raum;
            case libnki::GroupEffectType::bite:
                return &process_bite;
            case libnki::GroupEffectType::dirt:
                return &process_dirt;
            case libnki::GroupEffectType::freak:
                return &process_freak;
            case libnki::GroupEffectType::vibrato_chorus:
                return &process_vibrato_chorus;
            case libnki::GroupEffectType::wow_flutter:
                return &process_wow_flutter;
            case libnki::GroupEffectType::ep_preamps:
                return &process_ep_preamps;
            case libnki::GroupEffectType::fuzz:
                return &process_fuzz;
            case libnki::GroupEffectType::big_fuzz:
                return &process_big_fuzz;
            case libnki::GroupEffectType::stereo_tune:
                return &process_stereo_tune;
            case libnki::GroupEffectType::beat_masher:
                return &process_beat_masher;
            case libnki::GroupEffectType::beat_slicer:
                return &process_beat_slicer;
            case libnki::GroupEffectType::gater:
                return &process_gater;
            case libnki::GroupEffectType::reverse_grain:
                return &process_reverse_grain;
            case libnki::GroupEffectType::transpose_stretch:
                return &process_transpose_stretch;
            case libnki::GroupEffectType::super_fast_100:
                return &process_configured_stereo<
                        &EffectRuntime::super_fast_100_processor,
                        &EffectRuntime::super_fast_100_parameters,
                        &EffectRuntime::super_fast_100_sample_rate>;
            case libnki::GroupEffectType::chainsaw:
                return &process_configured_stereo<
                        &EffectRuntime::chainsaw_processor,
                        &EffectRuntime::chainsaw_parameters,
                        &EffectRuntime::chainsaw_sample_rate>;
            case libnki::GroupEffectType::skreamer_deluxe:
                return &process_configured_stereo<
                        &EffectRuntime::skreamer_deluxe_processor,
                        &EffectRuntime::skreamer_deluxe_parameters,
                        &EffectRuntime::skreamer_deluxe_sample_rate>;
            case libnki::GroupEffectType::kolor:
                return &process_configured_stereo<
                        &EffectRuntime::kolor_processor,
                        &EffectRuntime::kolor_parameters,
                        &EffectRuntime::kolor_sample_rate>;
            case libnki::GroupEffectType::reverb_delight:
                return &process_configured_stereo<
                        &EffectRuntime::reverb_delight_processor,
                        &EffectRuntime::reverb_delight_parameters,
                        &EffectRuntime::reverb_delight_sample_rate>;
            case libnki::GroupEffectType::hot_solo:
                return &process_configured_stereo<
                        &EffectRuntime::hot_solo_processor,
                        &EffectRuntime::hot_solo_parameters,
                        &EffectRuntime::hot_solo_sample_rate>;
            case libnki::GroupEffectType::jump:
                return &process_configured_stereo<
                        &EffectRuntime::jump_processor,
                        &EffectRuntime::jump_parameters,
                        &EffectRuntime::jump_sample_rate>;
            case libnki::GroupEffectType::plate_reverb:
                return &process_configured_stereo<
                        &EffectRuntime::plate_reverb_processor,
                        &EffectRuntime::plate_reverb_parameters,
                        &EffectRuntime::plate_reverb_sample_rate>;
            case libnki::GroupEffectType::twang:
                return &process_configured_stereo<
                        &EffectRuntime::twang_processor,
                        &EffectRuntime::twang_parameters,
                        &EffectRuntime::twang_sample_rate>;
            case libnki::GroupEffectType::ring_modulator:
                return &process_configured_stereo<
                        &EffectRuntime::ring_modulator_processor,
                        &EffectRuntime::ring_modulator_parameters,
                        &EffectRuntime::ring_modulator_sample_rate>;
            case libnki::GroupEffectType::d_stortion:
                return &process_configured_stereo<
                        &EffectRuntime::dstortion_processor,
                        &EffectRuntime::dstortion_parameters,
                        &EffectRuntime::dstortion_sample_rate>;
            case libnki::GroupEffectType::van51:
                return &process_configured_stereo<
                        &EffectRuntime::van51_processor,
                        &EffectRuntime::van51_parameters,
                        &EffectRuntime::van51_sample_rate>;
            case libnki::GroupEffectType::bass_invader:
                return &process_configured_stereo<
                        &EffectRuntime::bass_invader_processor,
                        &EffectRuntime::bass_invader_parameters,
                        &EffectRuntime::bass_invader_sample_rate>;
            case libnki::GroupEffectType::reverb:
                return &process_configured_stereo<
                        &EffectRuntime::legacy_reverb_processor,
                        &EffectRuntime::legacy_reverb_parameters,
                        &EffectRuntime::legacy_reverb_sample_rate>;
            case libnki::GroupEffectType::galois_reverb:
                return &process_configured_stereo<
                        &EffectRuntime::galois_reverb_processor,
                        &EffectRuntime::galois_reverb_parameters,
                        &EffectRuntime::galois_reverb_sample_rate>;
            case libnki::GroupEffectType::solid_bus_compressor:
                return &process_configured_stereo<
                        &EffectRuntime::solid_bus_processor,
                        &EffectRuntime::solid_bus_parameters,
                        &EffectRuntime::solid_bus_sample_rate>;
            case libnki::GroupEffectType::supercharger:
                return &process_configured_stereo<
                        &EffectRuntime::supercharger_processor,
                        &EffectRuntime::supercharger_parameters,
                        &EffectRuntime::supercharger_sample_rate>;
            case libnki::GroupEffectType::feedback_compressor:
                return &process_configured_stereo<
                        &EffectRuntime::feedback_compressor_processor,
                        &EffectRuntime::feedback_compressor_parameters,
                        &EffectRuntime::feedback_compressor_sample_rate>;
            case libnki::GroupEffectType::psyche_delay:
                return &process_configured_stereo<
                        &EffectRuntime::psyche_delay_processor,
                        &EffectRuntime::psyche_delay_parameters,
                        &EffectRuntime::psyche_delay_sample_rate>;
            case libnki::GroupEffectType::chorus:
                return &process_configured_stereo<
                        &EffectRuntime::chorus_processor,
                        &EffectRuntime::chorus_parameters,
                        &EffectRuntime::chorus_sample_rate>;
            case libnki::GroupEffectType::choral:
                return &process_configured_stereo<
                        &EffectRuntime::choral_processor,
                        &EffectRuntime::choral_parameters,
                        &EffectRuntime::choral_sample_rate>;
            case libnki::GroupEffectType::flanger:
                return &process_configured_stereo<
                        &EffectRuntime::flanger_processor,
                        &EffectRuntime::flanger_parameters,
                        &EffectRuntime::flanger_sample_rate>;
            case libnki::GroupEffectType::rotator:
                return &process_configured_stereo<
                        &EffectRuntime::rotator_processor,
                        &EffectRuntime::rotator_parameters,
                        &EffectRuntime::rotator_sample_rate>;
            case libnki::GroupEffectType::surround_panner:
                return &process_configured_stereo<
                        &EffectRuntime::surround_processor,
                        &EffectRuntime::surround_parameters,
                        &EffectRuntime::surround_sample_rate>;
            case libnki::GroupEffectType::compressor:
                return &process_compressor;
            case libnki::GroupEffectType::lo_fi:
                return &process_lo_fi;
            case libnki::GroupEffectType::shaper:
                return &process_shaper;
            case libnki::GroupEffectType::transparent_limiter:
                return &process_transparent_limiter;
            case libnki::GroupEffectType::transient_master:
                return &process_transient_master;
            default:
                return nullptr;
        }
    }
};

} // namespace ekphore::effect_runtime
