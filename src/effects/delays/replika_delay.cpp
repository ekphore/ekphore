// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "replika_delay.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <numbers>
#include <random>

namespace ekphore::replika_delay {
namespace {

float clamp01(const float value) { return std::clamp(value, 0.0F, 1.0F); }

std::uint32_t next_replika_constructor_ordinal() {
    // wow/flutter and both diffusers share this, seed once and NEVER on reset
    static std::atomic<std::uint32_t> ordinal{ std::random_device{}() };
    return ordinal.fetch_add(1, std::memory_order_relaxed);
}

float normalized_frequency(const float normalized, const float sample_rate) {
    const float frequency = 20.0F * std::pow(1000.0F, clamp01(normalized));
    return std::min(frequency, sample_rate * 0.45F);
}

std::array<float, 3> native_write_filter(const float angle) {
    const float square = angle * angle;
    const float cube = angle * square;
    const float tangent = std::fma(
            square * cube, 0.13333334F, std::fma(cube, 0.33333334F, angle));
    const float c0 = 1.0F / std::fma(tangent, 1.4F + tangent, 1.0F);
    const float c1 = tangent * c0;
    return { c0, c1, tangent * c1 };
}

std::array<float, 3> analog_filter(double omega, float rate, float damping) {
    const float angle = static_cast<float>(omega / static_cast<double>(rate));
    const float square = angle * angle, cube = square * angle;
    const float tangent = std::fma(
            square * cube, 0.13333334F, std::fma(cube, 0.33333334F, angle));
    const float c0 = 1.0F / std::fma(tangent, tangent + damping, 1.0F);
    const float c1 = tangent * c0;
    return { c0, c1, tangent * c1 };
}

std::pair<float, float> analog_tpt(float input, std::array<float, 2> &state,
        const std::array<float, 3> &c) {
    const float delta = input - state[1];
    const float band = std::fma(c[0], state[0], delta * c[1]);
    const float low = state[1] + std::fma(c[1], state[0], delta * c[2]);
    state = { std::fma(band, 2.0F, -state[0]), std::fma(low, 2.0F, -state[1]) };
    return { band, low };
}

float analog_exp(
        float position, const std::array<std::array<float, 2>, 2401> &table) {
    const int index = static_cast<int>(position);
    if (index < 0)
        return 0.0F;
    if (index >= 2400)
        return table[2400][0];
    return std::fma(position - static_cast<float>(index), table[index][1],
            table[index][0]);
}

} // namespace

void Processor::AnalogLane::configure(float rate, unsigned mode) {

    constexpr std::array<double, 4> omega1{ 28274.33388230814,
        12566.370614359172, 8482.300164692442, 25132.741228718343 };
    constexpr std::array<double, 4> omega2{ 26703.53755551324,
        10995.574287564275, 7853.981633974483, 21991.14857512855 };
    constexpr std::array<float, 4> k1{ 0.5F, 0.5F, 0.29999995F, 0.5F };
    constexpr std::array<float, 4> k2{ 1.3F, 1.2F, 1.3F, 1.3F };
    constexpr std::array<float, 4> out_band{ 11.22532F, 5.8979278F, 3.75F,
        15.9375F };
    constexpr std::array<float, 4> cubic{ 0.01F, 0.08F, 0.15F, 0.2F };
    constexpr std::array<float, 4> noise{ 0.005F, 0.015F, 0.035F, 0.01F };
    shape = effects::delays::replika_tape_constructor_shape();
    coefficients[0] = analog_filter(omega1[mode], rate, k1[mode]);
    coefficients[1] = analog_filter(omega2[mode], rate, k2[mode]);
    coefficients[2] =
            analog_filter(mode == 2 ? 439.822971502571 : 471.23889803846896,
                    rate, out_band[mode]);
    input_gain = mode == 3 ? 0.6F : 0.5F;
    output_gain = mode == 3 ? 1.6F : 2.0F;
    cubic_amount = cubic[mode];
    noise_amount = noise[mode];
    band_gain = out_band[mode];
    envelope_decay = static_cast<float>(std::exp(-235.61944901923448 / rate));
    output_decay = static_cast<float>(std::exp(-157.07963267948966 / rate));
    reset();
}

void Processor::AnalogLane::reset() {
    // only these, ADAA/cubic input/RNG stay
    filters = {};
    envelope = output_envelope = sidechain = 0.0F;
}

float Processor::AnalogLane::input(
        float value, const std::array<std::array<float, 2>, 2401> &table) {
    using namespace effects::delays::tape_saturator_detail;
    const float compressed =
            analog_exp(std::fma(sidechain, -9.965784F, 1200.0F), table) * value;
    const float db = std::log(std::abs(compressed) + 0.0001F) * 8.68589F;
    envelope = std::fma(envelope_decay, envelope - db, db);
    sidechain = envelope;
    rng = next_random(rng);
    const float random = std::fma(
            2.0F, static_cast<float>(rng - 1U) * bits(0x30000000), -1.0F);
    const float x =
            shape.scale * (input_gain * (compressed + noise_amount * random));
    const float primitive = shape.primitive(x), delta = x - previous_input;
    const float shaped = std::abs(delta) >= 0.0001F
                                 ? (primitive - previous_primitive) / delta
                                 : shape.direct(std::clamp(x, -1.0F, 1.0F));
    previous_input = x;
    previous_primitive = primitive;
    const float y = output_gain * shaped, square = y * y;
    const float cubic_primitive = (y * square) * 0.33333334F;
    const float cubic_delta = y - previous_cubic_input;
    const float derivative =
            cubic_delta > 0.0001F
                    ? (cubic_primitive - previous_cubic_primitive) / cubic_delta
                    : square;
    previous_cubic_input = y;
    previous_cubic_primitive = cubic_primitive;
    const float filtered1 = analog_tpt(
            std::fma(-cubic_amount, derivative, y), filters[0], coefficients[0])
                                    .second;
    return analog_tpt(filtered1, filters[1], coefficients[1]).second;
}

float Processor::AnalogLane::output(
        float value, const std::array<std::array<float, 2>, 2401> &table) {
    const float band = analog_tpt(value, filters[2], coefficients[2]).first;
    const float db = std::log(std::abs(value) + 0.0001F) * 8.68589F;
    output_envelope = std::fma(output_decay, output_envelope - db, db);
    return analog_exp(std::fma(output_envelope, 9.965784F, 1200.0F), table) *
           (band * band_gain);
}

void Processor::AnalogInterUnit::configure(
        float rate, float highcut, float lowcut) {
    // -10 dB high shelf, NOT a LP
    const float amplitude = std::pow(10.0F, -0.25F);
    const float frequency = std::fma(clamp01(highcut), 19800.0F, 200.0F);
    const float angle = static_cast<float>(
            (static_cast<double>(frequency) * std::numbers::pi_v<double>) /
            rate);
    const float square = angle * angle, cube = square * angle;
    const float tangent =
            std::sqrt(amplitude) * std::fma(square * cube, 0.13333334F,
                                           std::fma(cube, 0.33333334F, angle));
    const float c0 = 1.0F / std::fma(tangent, tangent + 1.6666666F, 1.0F);
    const float c1 = tangent * c0;
    shelf_coefficients = { c0, c1, tangent * c1 };
    shelf_weights = { amplitude * amplitude,
        (amplitude * (1.0F - amplitude)) / 0.6F, 1.0F - amplitude * amplitude };
    const float hp_frequency =
            std::pow(10.0F, std::fma(clamp01(lowcut), 2.0F, 1.0F));
    highpass_coefficients = analog_filter(
            static_cast<double>(hp_frequency) * std::numbers::pi_v<double>,
            rate, 2.0F);
    shelf_target = highcut == 1.0F ? 0.0F : 1.0F;
    highpass_target = lowcut == 0.0F ? 0.0F : 1.0F;
    // a new rate snaps the ramps and clears the filters
    shelf_mix = shelf_target;
    highpass_mix = highpass_target;
    decay = static_cast<float>(std::exp(
            -2.99573227355399 / (static_cast<double>(rate * 25.0F) / 1000.0)));
}

void Processor::AnalogInterUnit::reset() {
    shelf_state = {};
    highpass_state = {};
}

float Processor::AnalogInterUnit::process(float input, std::size_t channel) {
    // both lanes share one mix update per stereo sample, on the left call
    if (channel == 0) {
        shelf_mix = std::abs(shelf_target - shelf_mix) <= 0.001F
                            ? shelf_target
                            : std::fma(1.0F - decay, shelf_target,
                                      shelf_mix * decay);
        highpass_mix = std::abs(highpass_target - highpass_mix) <= 0.001F
                               ? highpass_target
                               : std::fma(1.0F - decay, highpass_target,
                                         highpass_mix * decay);
    }
    if (shelf_mix != 0.0F) {
        const auto [band, low] =
                analog_tpt(input, shelf_state[channel], shelf_coefficients);
        const float shelf = std::fma(shelf_weights[2], low,
                std::fma(shelf_weights[0], input, band * shelf_weights[1]));
        input = std::fma(input, 1.0F - shelf_mix, shelf_mix * shelf);
    }
    const auto [band, low] =
            analog_tpt(input, highpass_state[channel], highpass_coefficients);
    if (highpass_mix != 0.0F) {
        const float high = std::fma(-2.0F, band, input) - low;
        input = std::fma(input, 1.0F - highpass_mix, highpass_mix * high);
    }
    return input;
}

void Processor::ModernColour::configure(float rate, float saturation) {
    // the normal polynomial, not the constructor's zeros
    const float e2 = std::pow(10.F, -2.F), e225 = std::pow(10.F, -2.25F);
    const float e245 = std::pow(10.F, -2.45F);
    const float normalization = std::fma(e2, 5.F, 1.F + e225 * 3.F);
    polynomial = { std::fma(e2, 2.F, e245 * 8.F) / normalization,
        std::fma(e225, -4.F, e2 * -20.F) / normalization,
        e245 * -8.F / normalization, e2 * 16.F / normalization };
    const auto coefficients = [](float angle) {
        const float cube = angle * angle * angle;
        const float g = std::fma(angle * angle * cube, .13333334F,
                std::fma(cube, .33333334F, angle));
        const float a = 1.F / std::fma(g, g + 2.F, 1.F);
        const float b = g * a;
        return std::array<float, 3>{ a, b, g * b };
    };
    low_coefficients = coefficients(6927.212F / rate);
    high_coefficients = coefficients(15.707963F / rate);
    drive = std::pow(10.F, saturation);
    output_gain = std::pow(10.F, -saturation * .5F);
    low_state = {};
    high_state = {};
}

float Processor::ModernColour::process(float input, float saturation) {
    if (saturation == 0.F)
        return input;
    const float direct = std::tanh(input * drive);
    const float low = analog_tpt(direct, low_state, low_coefficients).second;
    const float square = low * low;
    float harmonic =
            std::fma(square * low, polynomial[1], square * polynomial[0]);
    harmonic = std::fma(square * square, polynomial[2], harmonic);
    harmonic = std::fma(square * square * low, polynomial[3], harmonic);
    const auto [band, filtered] =
            analog_tpt(harmonic, high_state, high_coefficients);
    const float shaped =
            output_gain * (direct + ((harmonic - band * 2.F) - filtered));
    return std::fma(saturation, shaped, (1.F - saturation) * input);
}

std::pair<float, float> Processor::read_modern_unit(std::size_t unit) {
    // integer base plus the cubic modulation
    auto &delay = units[unit];
    const float scale = static_cast<float>(sampling_rate) * .008F;
    const float depth = clamp01(params.depth);
    const float bias = -(depth * scale) - 2.F;
    std::array<float, 2> result{};
    const float sine = static_cast<float>(modern_imaginary);
    const float cosine = static_cast<float>(modern_real);
    for (unsigned c = 0; c < 2; ++c) {
        auto &line = delay.line[c];
        const float lfo = c ? std::fma(cosine, modern_stereo_mix,
                                      (1.F - modern_stereo_mix) * sine)
                            : sine;
        const float offset = std::min(0.F, std::fma(lfo * scale, depth, bias));
        // ties round away from zero; an integer cast would truncate instead
        double position = static_cast<double>(line.index) -
                          std::round(delay.delay_samples[c]) + offset;
        if (position < 0.)
            position += static_cast<double>(ring_capacity);
        if (position >= static_cast<double>(ring_capacity))
            position -= static_cast<double>(ring_capacity);
        const auto index = static_cast<std::size_t>(position);
        const float fraction = static_cast<float>(position - index);
        const float a = line.ring[(index - 1U) & delay_mask],
                    b = line.ring[index];
        const float cc = line.ring[(index + 1U) & delay_mask],
                    d = line.ring[(index + 2U) & delay_mask];
        const float half = a * -.5F, last = d * .5F;
        const float third = std::fma(cc, -1.5F, std::fma(b, 1.5F, half)) + last;
        const float second = std::fma(cc, 2.F, std::fma(b, -2.5F, a)) - last;
        result[c] =
                params.style == 4 && fraction == 0.F
                        ? b
                        : std::fma(std::fma(std::fma(fraction, third, second),
                                           fraction, std::fma(cc, .5F, half)),
                                  fraction, b);
        line.index = (line.index + 1U) & delay_mask;
        if (params.style == 4) {
            // two multiplies, NOT one with the product
            result[c] *= modern_input_gain[unit];
            result[c] *= modern_output_gain[unit];
        } else
            result[c] *= modern_input_gain[unit] * modern_output_gain[unit];
    }
    return transfer_channels(result[0], result[1], params.routing_enabled);
}

std::pair<float, float> Processor::limit_modern_return(
        float left, float right) {
    return limit_linked_return(left, right, modern_fast_db, modern_slow_db);
}

std::pair<float, float> Processor::limit_linked_return(
        float left, float right, float &fast_db, float &slow_db) {
    // limiter at (1,.5,1,0,0), unity gain, linked, no lookahead
    left *= .5F;
    right *= .5F;
    const float peak = std::max(std::abs(std::fma(left, 1.F, 1e-12F)),
            std::abs(std::fma(right, 1.F, 1e-12F)));
    const float db = std::log(peak) * 8.68589F;
    fast_db = std::fma(
            fast_db - db, db > fast_db ? modern_attack : modern_release, db);
    slow_db = std::fma(slow_db - db,
            db > slow_db ? modern_slow_attack : modern_slow_release, db);
    const float attenuation = std::min(0.F, -std::max(fast_db, slow_db));
    const float gain = analog_exp(
            std::fma(attenuation, 9.965784F, 1200.F), analog_exp_table);
    return { std::clamp(left * gain, -2.5F, 2.5F) * 2.F,
        std::clamp(right * gain, -2.5F, 2.5F) * 2.F };
}

std::pair<float, float> Processor::process_modern(float left, float right) {
    const double next =
            std::fma(modern_real, modern_sin, modern_imaginary * modern_cos);
    modern_real =
            std::fma(modern_imaginary, -modern_sin, modern_real * modern_cos);
    modern_imaginary = next;
    const float mix_target = params.style_modulation_enabled ? 1.F : 0.F;
    if (std::abs(mix_target - modern_stereo_mix) > .001F &&
            modern_mix_remaining > 1) {
        --modern_mix_remaining;
        modern_stereo_mix += modern_mix_step;
    } else
        modern_stereo_mix = mix_target;
    const float length = (sampling_rate * .001F) * 80.F;
    for (unsigned i = 0; i < 3; ++i) {
        if (modern_gain_remaining > 1) {
            if (std::abs(modern_output_target[i] - modern_output_gain[i]) >
                    .001F)
                modern_output_gain[i] += modern_output_target[i] / length;
            else
                modern_output_gain[i] = modern_output_target[i];
            if (i && std::abs(modern_input_target[i] - modern_input_gain[i]) >
                             .001F)
                modern_input_gain[i] += modern_input_target[i] / length;
            else
                modern_input_gain[i] = modern_input_target[i];
        } else {
            modern_input_gain[i] = modern_input_target[i];
            modern_output_gain[i] = modern_output_target[i];
        }
    }
    if (modern_gain_remaining)
        --modern_gain_remaining;
    if (params.style == 0) {
        left = modern_colour[0].process(left, clamp01(params.saturate));
        right = modern_colour[1].process(right, clamp01(params.saturate));
    }
    if (params.routing_enabled) {
        const float mono = stereo_average(left, right);
        left = params.routing_variant ? mono : 0.F;
        right = params.routing_variant ? 0.F : mono;
    }
    const auto write = [&](std::size_t i, float l, float r) {
        auto &lines = units[i].line;
        lines[0].ring[lines[0].index] = l;
        lines[1].ring[lines[1].index] = r;
    };
    write(0, left, right);
    const auto first = read_modern_unit(0);
    const auto delayed = read_modern_unit(2);
    const auto previous = limit_modern_return(delayed.first, delayed.second);
    float branch_l =
            analog_interunit[0].process(first.first + previous.first, 0);
    float branch_r =
            analog_interunit[0].process(first.second + previous.second, 1);
    if (params.style == 4) {
        const auto out = diffusers[0]->process({ branch_l, branch_r });
        branch_l = out[0];
        branch_r = out[1];
    }
    write(1, branch_l, branch_r);
    const auto second = read_modern_unit(1);
    float second_l = analog_interunit[1].process(second.first, 0);
    float second_r = analog_interunit[1].process(second.second, 1);
    if (params.style == 4) {
        const auto out = diffusers[1]->process({ second_l, second_r });
        second_l = out[0];
        second_r = out[1];
    }
    write(2, second_l, second_r);
    const float branch_gain = 1.F;
    const std::array<float, 2> wet{ std::fma(branch_gain, branch_l, second_l),
        std::fma(branch_gain, branch_r, second_r) };
    const float width = clamp01(params.width) * 2.F;
    const float middle = (wet[0] + wet[1]) * .5F,
                side = (wet[0] - wet[1]) * .5F * width;
    const float pan = clamp01(params.pan);
    return limit_linked_return(
            (middle + side) * std::min(1.F, 2.F * (1.F - pan)),
            (middle - side) * std::min(1.F, 2.F * pan), output_fast_db,
            output_slow_db);
}

void Processor::configure(
        const std::uint32_t sample_rate, const Parameters &parameters) {
    const auto previous_rate = sampling_rate;
    sampling_rate = std::max(sample_rate, 1U);
    params = parameters;
    if (params.style == 0 || params.style == 4) {

        const float rate = static_cast<float>(sampling_rate);
        const float pitch =
                std::fma(clamp01(params.rate), 79.72627F, -76.239456F);
        const float hz = static_cast<float>(
                std::exp2((static_cast<double>(pitch) - 69.0) / 12.0) * 440.0);
        // zero for Diffusion, its modulation is in the nested graphs
        // clang-format off
        const double angle = params.style == 4 ? 0.0 : static_cast<double>(hz) *
                2.0 * std::numbers::pi_v<double> / rate;
        // clang-format on
        modern_cos = std::cos(angle);
        modern_sin = std::sin(angle);
        modern_mix_remaining =
                static_cast<std::uint32_t>(std::round((rate * .001F) * 80.0F));
        modern_mix_step = (params.style_modulation_enabled ? 1.0F : 0.0F) /
                          ((rate * .001F) * 80.0F);
        for (auto &colour : modern_colour)
            colour.configure(rate, clamp01(params.saturate));
        for (auto &boundary : analog_interunit)
            boundary.configure(rate, params.high_cut, params.low_cut);
        modern_low_cpu = params.shuffle == 0.F && params.feel == .5F &&
                         params.accent == .5F && params.lr_offset == 0.F &&
                         !params.routing_enabled && !params.routing_variant;
        const float feedback =
                params.style == 4
                        ? static_cast<float>(static_cast<double>(
                                                     clamp01(params.feedback)) *
                                             1.2)
                        : clamp01(params.feedback) * 1.2F;
        modern_input_target = { 1.F,
            params.routing_enabled || modern_low_cpu ? 1.F : feedback,
            feedback };
        modern_output_target = { 1.F, 1.F, 1.F };
        const float accent = clamp01(params.accent);
        const float floor =
                static_cast<float>(std::exp2(1010.65008545 / 60.0 - 20.0));
        if (accent < .5F) {
            const float gain = std::max(floor, accent + accent);
            modern_output_target = { gain, 1.F / gain, gain };
        } else if (accent > .5F) {
            const float gain = std::max(floor, (1.F - accent) + (1.F - accent));
            modern_output_target = { 1.F / gain, gain, 1.F / gain };
        }
    }
    {
        // same setup for both, every style ends with it
        const float rate = static_cast<float>(sampling_rate);
        modern_attack = 1.F - 1.F / (rate * .005F);
        modern_release = 1.F - 1.F / (rate * 3.F);
        modern_slow_attack = 1.F - 1.F / (rate * 15.F);
        modern_slow_release = 1.F - 1.F / (rate * 3.F);
        // here, NOT pow per sample
        for (unsigned i = 0; i <= 2400; ++i) {
            analog_exp_table[i][0] = static_cast<float>(
                    std::exp2(static_cast<double>(i) / 60.0 - 20.0));
            if (i)
                analog_exp_table[i - 1][1] =
                        analog_exp_table[i][0] - analog_exp_table[i - 1][0];
        }
        analog_exp_table[2400][1] = 0.F;
    }
    if (params.style == 1) {
        // these six controls decide the shortcut, nothing else
        analog_low_cpu = params.shuffle == 0.0F && params.feel == 0.5F &&
                         params.accent == 0.5F && params.lr_offset == 0.0F &&
                         !params.routing_enabled && !params.routing_variant;
        for (auto &boundary : analog_interunit)
            boundary.configure(static_cast<float>(sampling_rate),
                    params.high_cut, params.low_cut);
        // the BBD modes are in reverse
        const unsigned quality = std::min(
                3U, static_cast<unsigned>(clamp01(params.bbd_type) * 4.0F));
        const unsigned mode = 3U - quality;
        constexpr std::array<double, 4> clock{ 0.5, 0.5, 1.0, 1.0 };
        analog_span = std::max(1.0, (sampling_rate * 0.25) * clock[quality]);
        analog_cubic = mode == 0;
        analog_gain_decay =
                std::exp(-2.995732273553991 / (sampling_rate * 0.005));
        // double fma, then float
        const float pitch = static_cast<float>(std::fma(
                static_cast<double>(clamp01(params.rate)),
                std::bit_cast<double>(std::uint64_t{ 0x4053ee7b46063b3d }),
                std::bit_cast<double>(std::uint64_t{ 0xc0530f5334889e36 })));
        const float rate = static_cast<float>(
                std::exp2((static_cast<double>(pitch) - 69.0) / 12.0) * 440.0);
        analog_step = (rate * 4.0F) / static_cast<float>(sampling_rate);
        const float length =
                (static_cast<float>(sampling_rate) * 0.001F) * 80.0F;
        analog_mix_length = static_cast<std::uint32_t>(std::round(length));
        analog_mix_step =
                (params.style_modulation_enabled ? 1.0F : 0.0F) / length;
        analog_exp_table[0][0] = 0x1p-20F;
        for (unsigned i = 1; i <= 2400; ++i) {
            analog_exp_table[i][0] = static_cast<float>(
                    std::exp2(static_cast<double>(i) / 60.0 - 20.0));
            analog_exp_table[i - 1][1] =
                    analog_exp_table[i][0] - analog_exp_table[i - 1][0];
        }
        analog_exp_table[2400][1] = 0.0F;

        // per unit, 1 in the middle
        const float accent = clamp01(params.accent);
        const float floor = analog_exp(1010.65008545F, analog_exp_table);
        float first_gain = 1.0F, second_gain = 1.0F;
        if (accent < 0.5F) {
            first_gain = std::max(floor, accent + accent);
            second_gain = 1.0F / first_gain;
        } else if (accent > 0.5F) {
            second_gain = std::max(floor, (1.0F - accent) + (1.0F - accent));
            first_gain = 1.0F / second_gain;
        }
        for (std::size_t i = 0; i < units.size(); ++i) {
            auto &unit = units[i];
            const float target = i == 1 ? second_gain : first_gain;
            if (target != unit.analog_output_target) {
                unit.analog_output_target = target;
                unit.analog_output_step =
                        (target - unit.analog_output_gain) / length;
                unit.analog_output_remaining = analog_mix_length;
            }
        }
        for (auto &unit : units)
            for (auto &line : unit.line) {
                // only when we build it
                if (!line.analog.initialized) {
                    std::random_device entropy;
                    line.analog.rng = entropy() % 0x7fffffffU;
                    if (line.analog.rng == 0)
                        line.analog.rng = 1;
                }
                line.analog.initialized = true;
                line.analog.configure(static_cast<float>(sampling_rate), mode);
            }
    }
    if (params.style == 2) {
        // Tape uses this too, it's not just for BBD
        analog_low_cpu = params.shuffle == 0.F && params.feel == .5F &&
                         params.accent == .5F && params.lr_offset == 0.F &&
                         !params.routing_enabled && !params.routing_variant;
        analog_gain_decay =
                std::exp(-2.995732273553991 / (sampling_rate * .005));
        const float length = (static_cast<float>(sampling_rate) * .001F) * 80.F;
        analog_mix_length = static_cast<std::uint32_t>(std::round(length));
        for (auto &boundary : analog_interunit)
            boundary.configure(static_cast<float>(sampling_rate),
                    params.high_cut, params.low_cut);
        const float accent = clamp01(params.accent);
        const float floor =
                static_cast<float>(std::exp2(1010.65008545 / 60.0 - 20.0));
        float first_gain = 1.F, second_gain = 1.F;
        if (accent < .5F) {
            first_gain = std::max(floor, accent + accent);
            second_gain = 1.F / first_gain;
        } else if (accent > .5F) {
            second_gain = std::max(floor, (1.F - accent) + (1.F - accent));
            first_gain = 1.F / second_gain;
        }
        for (std::size_t i = 0; i < units.size(); ++i) {
            auto &unit = units[i];
            const float target = i == 1 ? second_gain : first_gain;
            if (target != unit.analog_output_target) {
                unit.analog_output_target = target;
                unit.analog_output_step =
                        (target - unit.analog_output_gain) / length;
                unit.analog_output_remaining = analog_mix_length;
            }
        }
    }
    if (!tape_modulator) {
        // seeded once, one step per construction. NEVER reseed
        tape_modulator.emplace(effects::delays::ReplikaWowFlutter::initialized(
                constructor_wow_ordinal ? *constructor_wow_ordinal
                                        : next_replika_constructor_ordinal()));
        constructor_wow_ordinal.reset();
        using Clock = effects::delays::ReplikaDiffuserModulation;
        unsigned boundary = 0;
        for (auto &diffuser : diffusers) {
            const auto seed =
                    Clock::seed_for_ordinal(next_replika_constructor_ordinal());
            diffuser.emplace(false, params.diffusion_enabled,
                    clamp01(params.diffusion_amount),
                    Clock::configured_parameters(
                            static_cast<float>(sampling_rate),
                            Clock::scaled_size(clamp01(params.diffusion_size)),
                            clamp01(params.diffusion_mod)),
                    constructor_diffuser_seeds
                            ? (*constructor_diffuser_seeds)[boundary]
                            : seed);
            ++boundary;
        }
        constructor_diffuser_seeds.reset();
    } else {
        using Clock = effects::delays::ReplikaDiffuserModulation;
        for (auto &diffuser : diffusers) {
            if (previous_rate != sampling_rate)
                diffuser->set_sample_rate(static_cast<float>(sampling_rate),
                        clamp01(params.diffusion_mod));
            diffuser->set_mode(params.diffusion_enabled);
            diffuser->set_amount(clamp01(params.diffusion_amount));
            diffuser->set_modulation(static_cast<float>(sampling_rate),
                    Clock::scaled_size(clamp01(params.diffusion_size)),
                    clamp01(params.diffusion_mod));
        }
    }
    ring_capacity = 1;
    const auto required = static_cast<std::size_t>(sampling_rate * 5.5F) + 8U;
    while (ring_capacity < required)
        ring_capacity <<= 1U;
    delay_mask = ring_capacity - 1U;
    for (std::size_t unit_index = 0; unit_index < units.size(); ++unit_index)
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto &line = units[unit_index].line[channel];
            line.ring.assign(ring_capacity, 0.0F);
            // 20 kHz until the cursor first moves
            line.write_filter_coefficients = native_write_filter(
                    62831.85F / static_cast<float>(sampling_rate));
            if (params.style == 2) {
                std::uint32_t seed;
                if (line.tape_saturator)
                    seed = line.tape_saturator->state().rng;
                else if (constructor_tape_seeds) {
                    seed = (*constructor_tape_seeds)[unit_index][channel];
                    assert(seed > 0 && seed < 0x7fffffffU);
                } else {
                    // once at construction, NOT on reset or per sample
                    std::random_device entropy;
                    seed = entropy() % 0x7fffffffU;
                    if (seed == 0)
                        seed = 1;
                }
                auto state = effects::delays::replika_tape_initialized_state(
                        static_cast<float>(sampling_rate), seed);
                line.tape_saturator.emplace(state);
                const float age = std::sqrt(clamp01(params.tape_age));
                const float v = effects::delays::replika_tape_age_exp2(age);
                line.tape_saturator->set_age(age, v);
                const float saturation = clamp01(params.saturate);
                line.tape_saturator->set_saturate(
                        std::fma(saturation * saturation, 5.0F, 1.0F), v);
                line.tape_saturator->set_noise(params.tape_noise_enabled, v);
            }
        }
    if (params.style == 2)
        constructor_tape_seeds.reset();
    update_coefficients();
    reset();
}

void Processor::update_coefficients() {
    const float sample_rate = static_cast<float>(sampling_rate);
    if (params.style == 2)
        tape_modulator->set_tape_time(params.time, sample_rate);
    // still ms here
    const float base_delay =
            std::max(1.0F, params.time * sample_rate / 1000.0F);
    // shuffle/3, feel is +/-125 ms on the first unit only
    const float shuffle = clamp01(params.shuffle) / 3.0F;
    const float feel = ((1.0F - clamp01(params.feel)) * -2.0F + 1.0F) * 125.0F *
                       sample_rate / 1000.0F;
    // not in ping-pong
    const auto left_offsets = unit_lr_offsets(
            params.lr_offset, sampling_rate, params.routing_enabled);
    const std::array<float, unit_count> delays{ base_delay * (1.0F + shuffle) +
                                                        feel,
        base_delay * (1.0F - shuffle), base_delay * (1.0F + shuffle) };
    for (std::size_t unit = 0; unit < unit_count; ++unit) {
        const float center = std::clamp(
                delays[unit], 1.0F, static_cast<float>(ring_capacity - 4U));
        units[unit].delay_samples[0] = std::clamp(center + left_offsets[unit],
                1.0F, static_cast<float>(ring_capacity - 4U));
        units[unit].delay_samples[1] = std::clamp(center - left_offsets[unit],
                1.0F, static_cast<float>(ring_capacity - 4U));
        // only the oscillator changes per sample. keep the divide/min order,
        // NOT a reciprocal multiply
        const double span = params.style == 1
                                    ? analog_span
                                    : static_cast<double>(sampling_rate);
        for (unsigned channel = 0; channel < 2; ++channel)
            units[unit].line[channel].nominal_speed = std::min(50.5,
                    span / std::max(static_cast<double>(
                                            units[unit].delay_samples[channel]),
                                   1.0));
        units[unit].tape_pulse_scale = std::min(
                4.0, static_cast<double>(sampling_rate) /
                             std::max(static_cast<double>(
                                              units[unit].delay_samples[0]),
                                     1.0));
    }

    feedback_gain =
            params.style == 1
                    ? static_cast<float>(
                              static_cast<double>(clamp01(params.feedback)) *
                              1.2)
                    : clamp01(params.feedback) * 1.2F;
    const float rate_hz = 0.02F * std::pow(100.0F, clamp01(params.rate));
    modulation_step = rate_hz / sample_rate;
    modulation_depth_samples = clamp01(params.depth) * sample_rate * 0.012F;
    low_cut_coefficient = std::exp(
            -2.0F * std::numbers::pi_v<float> *
            normalized_frequency(params.low_cut, sample_rate) / sample_rate);
    high_cut_coefficient =
            1.0F - std::exp(-2.0F * std::numbers::pi_v<float> *
                            normalized_frequency(params.high_cut, sample_rate) /
                            sample_rate);
    const float release_ms = 10.0F + 1990.0F * clamp01(params.ducking_release);
    ducking_release_coefficient =
            std::exp(-1.0F / (sample_rate * release_ms * 0.001F));
}

void Processor::reset() {
    if (tape_modulator)
        tape_modulator->reset();
    tape_bias = 0.0F;
    tape_speed_multiplier = 1.0;
    tape_pulse_offset = 0.0;
    modulation_phase = 0.0F;
    modern_real = 1.0;
    modern_imaginary = 0.0;
    modern_stereo_mix = 0.F;
    modern_mix_remaining = static_cast<std::uint32_t>(
            std::round((sampling_rate * .001F) * 80.F));
    modern_gain_remaining = modern_mix_remaining;
    modern_input_gain = { 1.F, 0.F, 0.F };
    modern_output_gain = {};
    modern_fast_db = modern_slow_db = 0.F;
    output_fast_db = output_slow_db = 0.F;
    for (auto &colour : modern_colour) {
        colour.low_state = {};
        colour.high_state = {};
    }
    analog_triangle = { 0.0F, 1.0F };
    analog_direction = { 1.0F, 1.0F };
    ducking_envelope = 0.0F;
    high_pass_state = {};
    high_pass_input = {};
    for (auto &diffuser : diffusers)
        if (diffuser)
            diffuser->reset();
    for (auto &boundary : analog_interunit)
        boundary.reset();
    for (auto &unit : units) {
        unit.filter_state = {};
        unit.analog_gain = 0.0;
        unit.analog_stereo_mix = 0.0F;
        unit.analog_mix_remaining = analog_mix_length;
        for (auto &line : unit.line) {
            line.position = 0.0;
            line.fraction = 0.0;
            line.index = 0;
            line.crossed = 0;
            line.input_history = {};
            line.speed_history = { 1.0, 1.0, 1.0 };
            line.previous_read_position = 0.0;
            line.previous_read_offset = 0.0;
            line.previous_read_index = 0;
            line.anti_alias_state = {};
            line.write_filter_state = {};
            if (line.tape_saturator)
                line.tape_saturator->reset();
            line.analog.reset();
            std::fill(line.ring.begin(), line.ring.end(), 0.0F);
        }
    }
}

void Processor::write_line(MovingLine &line, const float raw_input) {
    float input = raw_input;
    if (params.style == 1)
        input = line.analog.input(input, analog_exp_table);
    if (params.style == 2) {
        input += tape_bias;
        input = line.tape_saturator->process(input);
    }
    if (params.style == 1 || params.style == 2) {
        // every sample BEFORE input_history, even if no cell was crossed
        const auto c = line.write_filter_coefficients;
        const float delta = input - line.write_filter_state[1];
        const float state0 =
                std::fma(c[0], line.write_filter_state[0], delta * c[1]);
        const float state1 =
                line.write_filter_state[1] +
                std::fma(c[1], line.write_filter_state[0], delta * c[2]);
        line.write_filter_state[0] =
                std::fma(state0, 2.0F, -line.write_filter_state[0]);
        line.write_filter_state[1] =
                std::fma(state1, 2.0F, -line.write_filter_state[1]);
        input = state1;
    }
    const auto previous = line.input_history;
    line.input_history = { input, previous[0], previous[1] };
    if (line.crossed == 0)
        return;

    if (params.style == 1 || params.style == 2) {
        const QuadraticWrite writer(input, previous[0], previous[1],
                line.speed_history[0], line.speed_history[1]);
        for (std::uint32_t offset = 0; offset < line.crossed; ++offset)
            line.ring[(line.index - offset) & delay_mask] =
                    writer.cell(line.fraction, offset);
        return;
    }

    // every crossed cell gets rebuilt from the last three inputs, it's NOT a
    // plain delay line write
    const float speed = static_cast<float>(line.speed_history[0]);
    const float minus_speed = -speed;
    const float prior_delta = static_cast<float>(
            static_cast<double>(minus_speed) - line.speed_history[1]);
    const float a = input / (minus_speed * prior_delta);
    const float b = previous[0] / ((minus_speed - prior_delta) * minus_speed);
    const float c = previous[1] / ((speed + prior_delta) * prior_delta);
    const float curve =
            prior_delta * -b - (prior_delta - speed) * a + speed * c;
    const float slope = c + a + b;
    for (std::uint32_t offset = 0; offset < line.crossed; ++offset) {
        const float t =
                static_cast<float>(line.fraction) + static_cast<float>(offset);
        line.ring[(line.index - offset) & delay_mask] =
                input - (curve - slope * t) * t;
    }
}

float Processor::read_line(MovingLine &line, const double modulation) {
    const bool analog = params.style == 1;
    const double read_offset =
            analog ? analog_span + 1.0
                   : static_cast<double>(sampling_rate) + modulation;
    double read_position = line.position - read_offset;
    if (read_position < 0.0)
        read_position += static_cast<double>(ring_capacity);
    const auto integer = static_cast<std::uint32_t>(read_position);

    // above 1x it low passes first, .5 cutoff and 1.4 damping
    const double effective_advance =
            line.speed_history[0] + (line.previous_read_offset - read_offset);
    const int crossed_read =
            static_cast<int>(line.previous_read_position + effective_advance) -
            static_cast<int>(line.previous_read_index);
    const float ratio = static_cast<float>(
            std::clamp(1.0 / std::abs(effective_advance), 0.2, 1.0));
    const bool tape = params.style == 2;
    const bool native_rounding = tape || analog;
    const float angle = native_rounding
                                ? (ratio * std::numbers::pi_v<float>)*0.5F
                                : 0.5F * ratio * std::numbers::pi_v<float>;
    const float angle3 = angle * angle * angle;
    // clang-format off
    const float tangent = native_rounding ? std::fma(angle * angle * angle3,
            0.13333334F, std::fma(angle3, 0.33333334F, angle)) : angle +
            angle3 / 3.0F + angle * angle * angle3 * 0.13333334F;
    // clang-format on
    const float coefficient0 =
            1.0F / (native_rounding ? std::fma(tangent, 1.4F + tangent, 1.0F)
                                    : (1.4F + tangent) * tangent + 1.0F);
    const float coefficient1 = tangent * coefficient0;
    const float coefficient2 = tangent * coefficient1;
    if (crossed_read > 0) {
        const int limited = std::min(crossed_read, 15);
        const int count = limited + std::max(2, 3 - limited) - 2;
        std::uint32_t cell = integer - static_cast<std::uint32_t>(limited) + 3U;
        for (int index = 0; index < count; ++index, ++cell) {
            float &sample = line.ring[cell & delay_mask];
            const float delta = sample - line.anti_alias_state[1];
            // clang-format off
            const float state0 = native_rounding ? std::fma(coefficient0,
                    line.anti_alias_state[0], delta * coefficient1) : delta *
                    coefficient1 + line.anti_alias_state[0] * coefficient0;
            // clang-format on
            // clang-format off
            const float state1 = native_rounding ? line.anti_alias_state[1] +
                    std::fma(coefficient1, line.anti_alias_state[0], delta *
                    coefficient2) : line.anti_alias_state[1] + delta *
                    coefficient2 + line.anti_alias_state[0] * coefficient1;
            // clang-format on
            line.anti_alias_state[0] =
                    native_rounding
                            ? std::fma(state0, 2.0F, -line.anti_alias_state[0])
                            : 2.0F * state0 - line.anti_alias_state[0];
            line.anti_alias_state[1] =
                    native_rounding
                            ? std::fma(state1, 2.0F, -line.anti_alias_state[1])
                            : 2.0F * state1 - line.anti_alias_state[1];
            sample = state1;
        }
    }
    line.previous_read_offset = read_offset;
    line.previous_read_index = integer & delay_mask;
    const float fraction = static_cast<float>(read_position - integer);
    // one path widens the rounded float back, the other keeps the double
    line.previous_read_position =
            (native_rounding ? static_cast<double>(fraction)
                             : read_position - static_cast<double>(integer)) +
            static_cast<double>(line.previous_read_index);

    const float first = line.ring[integer & delay_mask];
    // clang-format off
    const float output = (tape || (analog && analog_cubic)) ?
            cubic_read(line.ring[(integer - 1U) & delay_mask], first,
            line.ring[(integer + 1U) & delay_mask],
            line.ring[(integer + 2U) & delay_mask], fraction) : analog ?
            std::fma(fraction, line.ring[(integer + 1U) & delay_mask] - first,
            first) : first + (line.ring[(integer + 1U) & delay_mask] - first) *
            fraction;
    // clang-format on

    const double speed =
            line.nominal_speed * (tape            ? tape_speed_multiplier
                                         : analog ? analog_speed_multiplier
                                                  : 1.0);
    const auto next_integer = static_cast<std::uint32_t>(line.position + speed);
    line.crossed = next_integer - line.index;
    line.fraction = line.position + speed - next_integer;
    line.index = next_integer & delay_mask;
    line.position = line.fraction + static_cast<double>(line.index);
    line.speed_history = { speed, line.speed_history[0],
        line.speed_history[1] };
    if (native_rounding) {
        // from how far the cursor actually moved, NOT the delay time
        const float ratio =
                static_cast<float>(std::min(speed * 0.8, 0.9999999));
        line.write_filter_coefficients =
                native_write_filter((ratio * std::numbers::pi_v<float>)*0.5F);
    }
    return output;
}

std::pair<float, float> Processor::read_unit(
        const std::size_t unit, const float modulation) {
    auto &delay = units[unit];
    if (params.style == 1) {
        // starts at zero and ramps up over 5 ms, the oscillator mix over 80 ms
        if (1.0 - delay.analog_gain > 0.001)
            delay.analog_gain = std::fma(1.0 - analog_gain_decay, 1.0,
                    delay.analog_gain * analog_gain_decay);
        else
            delay.analog_gain = 1.0;
        const float target = params.style_modulation_enabled ? 1.0F : 0.0F;
        if (std::abs(target - delay.analog_stereo_mix) > 0.001F &&
                delay.analog_mix_remaining > 1) {
            --delay.analog_mix_remaining;
            delay.analog_stereo_mix += analog_mix_step;
        } else
            delay.analog_stereo_mix = target;
        const double depth = static_cast<double>(clamp01(params.depth)) * 0.1;
        analog_speed_multiplier =
                std::fma(depth, static_cast<double>(analog_triangle[0]), 1.0);
        const float left_read = read_line(delay.line[0], 0.0);
        const double right_lfo =
                std::fma(static_cast<double>(delay.analog_stereo_mix),
                        static_cast<double>(analog_triangle[1]),
                        static_cast<double>(1.0F - delay.analog_stereo_mix) *
                                analog_triangle[0]);
        analog_speed_multiplier = std::fma(depth, right_lfo, 1.0);
        const float right_read = read_line(delay.line[1], 0.0);
        const float gain = static_cast<float>(delay.analog_gain);
        if (std::abs(delay.analog_output_target - delay.analog_output_gain) >
                        0.001F &&
                delay.analog_output_remaining > 1) {
            --delay.analog_output_remaining;
            delay.analog_output_gain += delay.analog_output_step;
        } else
            delay.analog_output_gain = delay.analog_output_target;
        // AFTER the BBD expansion and BEFORE ping-pong, not on write
        const float input_gain =
                (unit == 2 || (unit == 1 && !params.routing_enabled &&
                                      !analog_low_cpu))
                        ? feedback_gain
                        : 1.0F;
        return transfer_channels(delay.line[0].analog.output(
                                         left_read * gain, analog_exp_table) *
                                         input_gain * delay.analog_output_gain,
                delay.line[1].analog.output(
                        right_read * gain, analog_exp_table) *
                        input_gain * delay.analog_output_gain,
                params.routing_enabled);
    }
    // both channels use the LEFT speed for this
    const double read_modulation =
            params.style == 2 ? tape_pulse_offset * delay.tape_pulse_scale
                              : static_cast<double>(modulation);
    const float left = read_line(delay.line[0], read_modulation);
    const float right = read_line(delay.line[1], read_modulation);
    if (params.style == 2 && !analog_low_cpu) {
        // fade and gains BEFORE the channel transfer, moving the feedback
        // before the tape writer changes the saturation
        if (1.0 - delay.analog_gain > .001)
            delay.analog_gain = std::fma(1.0 - analog_gain_decay, 1.0,
                    delay.analog_gain * analog_gain_decay);
        else
            delay.analog_gain = 1.0;
        if (std::abs(delay.analog_output_target - delay.analog_output_gain) >
                        .001F &&
                delay.analog_output_remaining > 1) {
            --delay.analog_output_remaining;
            delay.analog_output_gain += delay.analog_output_step;
        } else
            delay.analog_output_gain = delay.analog_output_target;
        const float gain = static_cast<float>(delay.analog_gain);
        const float input_gain =
                (unit == 2 || (unit == 1 && !params.routing_enabled))
                        ? feedback_gain
                        : 1.F;
        return transfer_channels(
                (left * gain) * input_gain * delay.analog_output_gain,
                (right * gain) * input_gain * delay.analog_output_gain,
                params.routing_enabled);
    }
    return transfer_channels(left, right, params.routing_enabled);
}

std::pair<float, float> Processor::transfer_channels(
        const float left, const float right, const bool ping_pong) {
    if (!ping_pong)
        return { left, right };
    // after the read on all three units, and in the feedback too, not just the
    // output
    const float cosine =
            static_cast<float>(std::cos(std::numbers::pi_v<double> * 0.5));
    const float sine =
            static_cast<float>(std::sin(std::numbers::pi_v<double> * 0.5));
    return { right * sine + left * cosine, left * sine + right * cosine };
}

void Processor::write_unit(
        const std::size_t unit, const float left, const float right) {
    auto &delay = units[unit];
    write_line(delay.line[0], left);
    write_line(delay.line[1], right);
}

float Processor::style_sample(float input) {
    const float saturation = clamp01(params.saturate);
    switch (std::clamp(params.style, 0, 4)) {
        case 0: {
            const float drive = 1.0F + saturation * 5.0F;
            return std::tanh(input * drive) / std::tanh(drive);
        }
        case 1: {
            return input; // BBD colour is per unit, later
        }
        case 2: {
            // once BEFORE routing and the first write, NOT a tanh and NOT
            // again in the feedback
            return tape_ingress(input, saturation);
        }
        case 3: {
            const float levels = 64.0F + 960.0F * clamp01(params.quality);
            return std::round(input * levels) / levels;
        }
        case 4:
            // straight into unit 1, only the returns go through the diffusers
            return input;
    }
    return input;
}

std::pair<float, float> Processor::process(
        const float left, const float right) {
    if (ring_capacity == 0)
        configure(sampling_rate, params);
    // TODO: we haven't checked the low-CPU and bypass transitions yet
    if ((params.style == 0 && !modern_low_cpu) || params.style == 4)
        return process_modern(left, right);
    const float shared = tape_modulator->process();
    if (params.style == 2) {
        const float age = std::sqrt(clamp01(params.tape_age));
        const float pulse = tape_modulator->get<float>(4);
        tape_bias = (age * 0.25F) * std::fma(shared, 0.01F, pulse);
        const float flutter = clamp01(params.flutter);
        const float amount = (flutter * flutter) * flutter;
        tape_speed_multiplier = std::fma(static_cast<double>(amount) * 0.1,
                static_cast<double>(shared), 1.0);
        const float pulse_amount =
                static_cast<float>((static_cast<double>(age) * age) * age) *
                500.0F;
        tape_pulse_offset =
                static_cast<double>((pulse_amount + pulse_amount) * pulse);
    }
    if (params.style == 1)
        for (std::size_t i = 0; i < 2; ++i) {
            analog_triangle[i] = std::fma(
                    analog_step, analog_direction[i], analog_triangle[i]);
            if (analog_triangle[i] >= 1.0F) {
                analog_triangle[i] = 1.0F;
                analog_direction[i] = -1.0F;
            } else if (analog_triangle[i] <= -1.0F) {
                analog_triangle[i] = -1.0F;
                analog_direction[i] = 1.0F;
            }
        }
    const float detector = std::max(std::abs(left), std::abs(right));
    ducking_envelope = detector > ducking_envelope
                               ? detector
                               : detector + (ducking_envelope - detector) *
                                                    ducking_release_coefficient;
    const float threshold = 1.0F - clamp01(params.ducking_sensitivity);
    const float reduction = ducking_envelope > threshold
                                    ? 1.0F - clamp01(params.ducking_amount)
                                    : 1.0F;

    modulation_phase += modulation_step;
    modulation_phase -= std::floor(modulation_phase);
    float modulation = 0.0F;
    if (params.style_modulation_enabled && params.style != 2 &&
            params.style != 1)
        modulation =
                std::sin(modulation_phase * 2.0F * std::numbers::pi_v<float>) *
                modulation_depth_samples;
    // Tape already has its wow/flutter, don't add another tone

    float input_left = style_sample(left);
    float input_right = style_sample(right);
    if (params.routing_enabled) {
        // (0, 1) for mode 1, (1, 0) for mode 2, on the mono sum before unit 1
        const float mono = stereo_average(input_left, input_right);
        input_left = params.routing_variant ? mono : 0.0F;
        input_right = params.routing_variant ? 0.0F : mono;
    }
    write_unit(0, input_left, input_right);
    const auto first = read_unit(0, modulation);
    const auto previous_third = read_unit(2, modulation * 0.25F);
    const bool native_units =
            params.style == 1 || (params.style == 2 && !analog_low_cpu);
    const float branch_left =
            native_units ? analog_interunit[0].process(
                                   first.first + previous_third.first, 0)
                         : first.first + previous_third.first;
    const float branch_right =
            native_units ? analog_interunit[0].process(
                                   first.second + previous_third.second, 1)
                         : first.second + previous_third.second;
    write_unit(1, branch_left, branch_right);
    const auto second = read_unit(1, modulation * -0.5F);
    const float second_left =
            native_units ? analog_interunit[1].process(second.first, 0)
                         : second.first;
    const float second_right =
            native_units ? analog_interunit[1].process(second.second, 1)
                         : second.second;
    // unit 3 goes back into unit 1 here, NOT the final wet output
    write_unit(2, native_units ? second_left : second_left * feedback_gain,
            native_units ? second_right : second_right * feedback_gain);

    const float accent = native_units ? 1.0F : clamp01(params.accent);
    std::array<float, 2> wet{ branch_left * accent + second_left,
        branch_right * accent + second_right };
    for (std::size_t channel = 0; channel < 2; ++channel) {
        if (native_units) {
            wet[channel] *= reduction;
            continue;
        }
        const float high_pass =
                low_cut_coefficient * (high_pass_state[channel] + wet[channel] -
                                              high_pass_input[channel]);
        high_pass_input[channel] = wet[channel];
        high_pass_state[channel] = high_pass;
        units[0].filter_state[channel] +=
                high_cut_coefficient *
                (high_pass - units[0].filter_state[channel]);
        wet[channel] = units[0].filter_state[channel] * reduction;
    }
    const float pan = clamp01(params.pan);
    const float width = clamp01(params.width) * 2.0F;
    const float middle = (wet[0] + wet[1]) * 0.5F;
    const float side = (wet[0] - wet[1]) * 0.5F * width;
    const float out_left =
            (middle + side) * std::min(1.0F, 2.0F * (1.0F - pan));
    const float out_right = (middle - side) * std::min(1.0F, 2.0F * pan);
    return limit_linked_return(
            out_left, out_right, output_fast_db, output_slow_db);
}

} // namespace ekphore::replika_delay
