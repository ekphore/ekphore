// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../effects/utilities/routing.h"
#include <algorithm>

namespace ekphore::dsp {

// reserve before audio starts, a bigger direct render can still allocate
struct RenderBuffers {
    routing::StereoBlock mixed;
    // skips program inserts, sends/main and program gain
    routing::StereoBlock direct;
    std::vector<routing::StereoBlock> programs;
    std::vector<std::vector<routing::StereoBlock>> buses;
    std::vector<std::array<routing::StereoBlock, 8>> sends;

    template <class BusCount>
    void prepare(
            std::size_t program_count, BusCount bus_count, std::size_t frames) {
        programs.resize(program_count);
        buses.resize(program_count);
        sends.resize(program_count);
        for (std::size_t i = 0; i < program_count; ++i)
            buses[i].resize(bus_count(i));
        each([frames](auto &block) {
            block.left.reserve(frames);
            block.right.reserve(frames);
        });
    }

    // clear activity too, or a silent block still runs sends/effects
    void clear(std::size_t frames) {
        each([frames](auto &block) {
            block.left.resize(frames);
            block.right.resize(frames);
            std::fill(block.left.begin(), block.left.end(), 0.0F);
            std::fill(block.right.begin(), block.right.end(), 0.0F);
            block.active = false;
        });
    }

    // activity means a voice started, don't guess it from nonzero samples
    void place(const RenderBuffers &source, std::size_t offset) {
        const auto place_block = [offset](auto &dst, const auto &block) {
            std::copy(block.left.begin(), block.left.end(),
                    dst.left.begin() + offset);
            std::copy(block.right.begin(), block.right.end(),
                    dst.right.begin() + offset);
            dst.active |= block.active;
        };
        place_block(direct, source.direct);
        for (std::size_t p = 0; p < source.programs.size(); ++p) {
            place_block(programs[p], source.programs[p]);
            for (std::size_t b = 0; b < source.buses[p].size(); ++b)
                place_block(buses[p][b], source.buses[p][b]);
            for (std::size_t s = 0; s < source.sends[p].size(); ++s)
                place_block(sends[p][s], source.sends[p][s]);
        }
    }

  private:
    template <class Function>
    void each(Function &&function) {
        function(mixed);
        function(direct);
        for (auto &block : programs)
            function(block);
        for (auto &program : buses)
            for (auto &block : program)
                function(block);
        for (auto &program : sends)
            for (auto &block : program)
                function(block);
    }
};

} // namespace ekphore::dsp
