// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace ekphore::dsp {

// one thread per side, stop both before destroying this. prepare() and commit
// can't throw, so put errors in the Job
template <class Job, std::size_t Capacity>
class PreparedJobSlots {
    static_assert(Capacity > 0);
    enum class State : unsigned char { free, pending, ready, retired };
    static_assert(std::atomic<State>::is_always_lock_free);
    static_assert(noexcept(std::declval<Job &>().prepare()));
    struct Slot {
        std::unique_ptr<Job> job;
        std::atomic<State> state{ State::free };
    };

  public:
    // if the slot is busy, leave the job with the caller so it can retry
    bool submit(std::unique_ptr<Job> &job) noexcept {
        if (!job)
            return false;
        auto &slot = slots[submit_index];
        if (slot.state.load(std::memory_order_acquire) != State::free)
            return false;
        slot.job.swap(job);
        slot.state.store(State::pending, std::memory_order_release);
        submit_index = next(submit_index);
        return true;
    }

    template <class Commit>
    std::size_t commit_ready(Commit &&commit) noexcept {
        static_assert(std::is_nothrow_invocable_v<Commit &, Job &>);
        std::size_t count = 0;
        for (; count < Capacity; ++count) {
            auto &slot = slots[commit_index];
            if (slot.state.load(std::memory_order_acquire) != State::ready)
                break;
            commit(*slot.job);
            slot.state.store(State::retired, std::memory_order_release);
            commit_index = next(commit_index);
        }
        return count;
    }

    // reclaim first so we can reuse the slots
    std::size_t service_worker() noexcept {
        std::size_t work = 0;
        for (auto &slot : slots) {
            if (slot.state.load(std::memory_order_acquire) != State::retired)
                continue;
            slot.job.reset();
            slot.state.store(State::free, std::memory_order_release);
            ++work;
        }
        for (std::size_t i = 0; i < Capacity; ++i) {
            auto &slot = slots[prepare_index];
            if (slot.state.load(std::memory_order_acquire) != State::pending)
                break;
            (void)slot.job->prepare();
            slot.state.store(State::ready, std::memory_order_release);
            prepare_index = next(prepare_index);
            ++work;
        }
        return work;
    }

    // the submitter has to be stopped already. uncommitted jobs get dropped
    // here without callbacks
    void discard_worker_shutdown() noexcept {
        for (auto &slot : slots) {
            slot.job.reset();
            slot.state.store(State::free, std::memory_order_relaxed);
        }
    }

  private:
    static constexpr std::size_t next(std::size_t i) noexcept {
        return i + 1 == Capacity ? 0 : i + 1;
    }
    std::array<Slot, Capacity> slots{};
    std::size_t submit_index{}, commit_index{}; // submitting thread only
    std::size_t prepare_index{};                // worker only
};

// NOT on audio, and stop the engine before destroying it. we poll so audio
// never has to wake the worker, costs some latency
template <class Job, std::size_t Capacity>
class PreparedJobWorker {
  public:
    PreparedJobWorker()
        : worker([this](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  if (slots.service_worker() == 0)
                      std::this_thread::sleep_for(
                              std::chrono::milliseconds{ 1 });
              }
              slots.discard_worker_shutdown();
          }) {}

    ~PreparedJobWorker() {
        worker.request_stop();
        worker.join(); // can wait for a decode to finish
    }
    PreparedJobWorker(const PreparedJobWorker &) = delete;
    PreparedJobWorker &operator=(const PreparedJobWorker &) = delete;

    bool submit(std::unique_ptr<Job> &job) noexcept {
        return slots.submit(job);
    }

    template <class Commit>
    std::size_t commit_ready(Commit &&commit) noexcept {
        return slots.commit_ready(std::forward<Commit>(commit));
    }

  private:
    PreparedJobSlots<Job, Capacity> slots;
    std::jthread worker;
};

} // namespace ekphore::dsp
