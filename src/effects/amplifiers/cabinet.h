// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ekphore::cabinet::tables {

// just the four ints, no UI stuff
inline constexpr std::array<std::array<std::int32_t, 4>, 11> setups{ {
        { { 3, 4, 0, 0 } },
        { { 5, 4, 0, 1 } },
        { { 6, 4, 0, 2 } },
        { { 6, 4, 4, 2 } },
        { { 9, 4, 0, 3 } },
        { { 9, 4, 3, 3 } },
        { { 13, 4, 0, 4 } },
        { { 13, 4, 3, 4 } },
        { { 18, 4, 0, 5 } },
        { { 18, 4, 4, 5 } },
        { { 8, 1, 0, 6 } },
} };

// index as [setup[0]][setup[1]][setup[2]], with dimensions 19 x 5 x 5
inline constexpr std::array<std::uint16_t, 19U * 5U * 5U> filter_map{ { 246U,
        247U, 248U, 249U, 262U, 250U, 251U, 252U, 253U, 262U, 254U, 255U, 256U,
        257U, 262U, 258U, 259U, 260U, 261U, 262U, 263U, 264U, 265U, 266U, 262U,
        267U, 268U, 269U, 270U, 283U, 271U, 272U, 273U, 274U, 283U, 275U, 276U,
        277U, 278U, 283U, 279U, 280U, 281U, 282U, 283U, 284U, 285U, 286U, 287U,
        283U, 120U, 121U, 122U, 123U, 136U, 124U, 125U, 126U, 127U, 136U, 128U,
        129U, 130U, 131U, 136U, 132U, 133U, 134U, 135U, 136U, 137U, 138U, 139U,
        140U, 136U, 141U, 142U, 143U, 144U, 157U, 145U, 146U, 147U, 148U, 157U,
        149U, 150U, 151U, 152U, 157U, 153U, 154U, 155U, 156U, 157U, 158U, 159U,
        160U, 161U, 157U, 183U, 184U, 185U, 186U, 199U, 187U, 188U, 189U, 190U,
        199U, 191U, 192U, 193U, 194U, 199U, 195U, 196U, 197U, 198U, 199U, 200U,
        201U, 202U, 203U, 199U, 204U, 205U, 206U, 207U, 220U, 208U, 209U, 210U,
        211U, 220U, 212U, 213U, 214U, 215U, 220U, 216U, 217U, 218U, 219U, 220U,
        221U, 222U, 223U, 224U, 220U, 162U, 163U, 164U, 165U, 178U, 166U, 167U,
        168U, 169U, 178U, 170U, 171U, 172U, 173U, 178U, 174U, 175U, 176U, 177U,
        178U, 179U, 180U, 181U, 182U, 178U, 0U, 1U, 2U, 3U, 3U, 4U, 5U, 6U, 7U,
        7U, 8U, 9U, 10U, 11U, 11U, 12U, 13U, 14U, 15U, 15U, 16U, 17U, 18U, 19U,
        19U, 288U, 289U, 290U, 291U, 304U, 292U, 293U, 294U, 295U, 304U, 296U,
        297U, 298U, 299U, 304U, 300U, 301U, 302U, 303U, 304U, 305U, 306U, 307U,
        308U, 304U, 225U, 226U, 227U, 228U, 241U, 229U, 230U, 231U, 232U, 241U,
        233U, 234U, 235U, 236U, 241U, 237U, 238U, 239U, 240U, 241U, 242U, 243U,
        244U, 245U, 241U, 40U, 41U, 42U, 43U, 43U, 44U, 45U, 46U, 47U, 47U, 48U,
        49U, 50U, 51U, 51U, 52U, 53U, 54U, 55U, 55U, 56U, 57U, 58U, 59U, 59U,
        60U, 61U, 62U, 63U, 63U, 64U, 65U, 66U, 67U, 67U, 68U, 69U, 70U, 71U,
        71U, 72U, 73U, 74U, 75U, 75U, 76U, 77U, 78U, 79U, 79U, 80U, 81U, 82U,
        83U, 83U, 84U, 85U, 86U, 87U, 87U, 88U, 89U, 90U, 91U, 91U, 92U, 93U,
        94U, 95U, 95U, 96U, 97U, 98U, 99U, 99U, 20U, 21U, 22U, 23U, 23U, 24U,
        25U, 26U, 27U, 27U, 28U, 29U, 30U, 31U, 31U, 32U, 33U, 34U, 35U, 35U,
        36U, 37U, 38U, 39U, 39U, 100U, 101U, 102U, 103U, 103U, 104U, 105U, 106U,
        107U, 107U, 108U, 109U, 110U, 111U, 111U, 112U, 113U, 114U, 115U, 115U,
        116U, 117U, 118U, 119U, 119U, 350U, 351U, 352U, 353U, 353U, 354U, 355U,
        356U, 357U, 357U, 358U, 359U, 360U, 361U, 361U, 362U, 363U, 364U, 365U,
        365U, 366U, 367U, 368U, 369U, 369U, 412U, 413U, 414U, 415U, 432U, 416U,
        417U, 418U, 419U, 432U, 420U, 421U, 422U, 423U, 432U, 424U, 425U, 426U,
        427U, 432U, 428U, 429U, 430U, 431U, 432U, 329U, 330U, 331U, 332U, 349U,
        333U, 334U, 335U, 336U, 349U, 337U, 338U, 339U, 340U, 349U, 341U, 342U,
        343U, 344U, 349U, 345U, 346U, 347U, 348U, 349U, 391U, 392U, 393U, 394U,
        411U, 395U, 396U, 397U, 398U, 411U, 399U, 400U, 401U, 402U, 411U, 403U,
        404U, 405U, 406U, 411U, 407U, 408U, 409U, 410U, 411U } };

// 23 records of eight floats each, then the gain
#include "cabinet_responses.inc"

inline float response_value(const std::size_t type, const std::size_t word) {
    return std::bit_cast<float>(response_bits[type][word]);
}

inline constexpr std::uint16_t response_index(const std::size_t type) {
    const auto &setup =
            setups[type < setups.size() ? type : setups.size() - 1U];
    const auto offset = static_cast<std::size_t>(setup[0]) * 25U +
                        static_cast<std::size_t>(setup[1]) * 5U +
                        static_cast<std::size_t>(setup[2]);
    return filter_map[offset];
}

} // namespace ekphore::cabinet::tables

namespace ekphore::cabinet {

enum class CoefficientPublication { template_snapshot, runtime_copy };

struct Parameters {
    float size{};
    float air{};
    float treble{};
    float bass{};
    std::int32_t type{};
    CoefficientPublication publication{
        CoefficientPublication::template_snapshot
    };
};

struct Coefficients {
    std::array<float, 23> numerator_sum{};
    std::array<float, 23> numerator_product{};
    std::array<float, 23> denominator_sum{};
    std::array<float, 23> denominator_product{};
    std::array<float, 4> tone{};
    std::array<float, 4> stereo_gain{};
    std::uint16_t response{};
    float transformed_size{};
};

Coefficients coefficients(
        std::uint32_t sample_rate, const Parameters &parameters);

class Processor {
  public:
    void configure(std::uint32_t sample_rate, const Parameters &parameters);
    // between spans only
    void publish(const Parameters &parameters);
    // once per span, then process() exactly frames times
    void begin_span(std::size_t frames);
    void reset();
    void process(float &left, float &right);
    std::uint16_t phase() const { return current_phase; }
    std::int32_t remaining() const { return hold_remaining; }
    // only this span, you still have to check the input
    bool has_signal() const { return signal_active; }

  private:
    struct SectionState {
        float first{};
        float second{};
    };

    struct ChannelTail {
        float pending_even{};
        std::array<SectionState, 12> even{};
        std::array<SectionState, 12> odd{};
        float feedback_smoother{};
        float feedback_output{};
    };

    float run_section(float input, SectionState &state, float a, float b,
            float c, float d);
    float run_cascade(float input, std::size_t channel);
    void control_tick();
    void prepare_chunk();
    void finish_span();

    Coefficients current_coefficients{};
    Coefficients targets{};
    Parameters pending{};
    CoefficientPublication publication{
        CoefficientPublication::template_snapshot
    };
    std::uint32_t processing_rate{};
    std::int32_t selected_model{ -1 };
    std::int32_t response{ -1 };
    float transformed_size{ -1.0F };
    bool reset_marker{ true };
    bool dirty{};
    std::uint16_t current_phase{};
    std::int32_t hold_remaining{};
    std::int32_t ramp_ticks{};
    std::int32_t ramp_remaining{};
    float ramp_scale{};
    std::array<float, 4> gain_steps{};
    std::array<float, 4> tone_steps{};
    std::size_t control_remaining{};
    std::size_t chunk_remaining{};
    std::size_t span_frames{};
    std::size_t span_remaining{};
    float fade_gain{ 1.0F };
    float fade_step{};
    std::array<float, 4> activity_peaks{};
    bool signal_active{};
    std::array<ChannelTail, 2> channels{};
    std::array<std::vector<float>, 2> delays{};
    std::array<std::uint32_t, 8> tap_offsets{};
    std::uint32_t delay_mask{};
    std::uint32_t delay_position{};
    float first_smoothing_step{};
    float second_smoothing_step{};
};

} // namespace ekphore::cabinet
