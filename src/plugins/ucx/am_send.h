/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef NIXL_SRC_PLUGINS_UCX_AM_SEND_H
#define NIXL_SRC_PLUGINS_UCX_AM_SEND_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

extern "C" {
#include <ucp/api/ucp.h>
}

namespace nixl::ucx {

// Number of AM sends in flight whose request the send callback frees. Each
// send in flight and each reader keeps its own shared_ptr to it, so a late
// callback or a test can still use it after the endpoint or worker is gone.
class amSendStats {
public:
    [[nodiscard]] size_t
    pending() const noexcept {
        return pending_;
    }

    void
    add() noexcept {
        ++pending_;
    }

    void
    remove() noexcept {
        --pending_;
    }

private:
    std::atomic<size_t> pending_{0};
};

// Counts one AM send as pending on its endpoint and on its worker while it
// exists.
class amPendingStats {
public:
    amPendingStats(std::shared_ptr<amSendStats> ep_stats,
                   std::shared_ptr<amSendStats> worker_stats) noexcept
        : epStats_(std::move(ep_stats)),
          workerStats_(std::move(worker_stats)) {
        epStats_->add();
        workerStats_->add();
    }

    ~amPendingStats() {
        epStats_->remove();
        workerStats_->remove();
    }

    amPendingStats(const amPendingStats &) = delete;
    amPendingStats &
    operator=(const amPendingStats &) = delete;

private:
    const std::shared_ptr<amSendStats> epStats_;
    const std::shared_ptr<amSendStats> workerStats_;
};

// Payload of an AM send in flight. A counted send is pending while its context
// exists, and its callback releases the request.
class amSendCtx {
public:
    explicit amSendCtx(std::string &&data) noexcept : payload_(std::move(data)) {}

    amSendCtx(std::string &&data,
              std::shared_ptr<amSendStats> ep_stats,
              std::shared_ptr<amSendStats> worker_stats) noexcept
        : payload_(std::move(data)),
          pendingStats_(std::in_place, std::move(ep_stats), std::move(worker_stats)) {}

    [[nodiscard]] const std::string &
    payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] bool
    counted() const noexcept {
        return pendingStats_.has_value();
    }

private:
    const std::string payload_;
    const std::optional<amPendingStats> pendingStats_;
};

// Saturate before converting milliseconds to the steady clock's duration:
// a valid milliseconds::rep can overflow that conversion or time-point addition.
[[nodiscard]] inline std::chrono::steady_clock::time_point
amDrainDeadline(std::chrono::milliseconds timeout) noexcept {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    if (timeout <= std::chrono::milliseconds::zero()) {
        return now;
    }
    const auto max_timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max());
    if (timeout >= max_timeout) {
        return clock::time_point::max();
    }
    const auto delta = std::chrono::duration_cast<clock::duration>(timeout);
    return (now >= clock::time_point::max() - delta) ? clock::time_point::max() : now + delta;
}

// How long a worker and its endpoints wait for their pending AM sends at
// teardown. Teardown can also give all of them one deadline.
class amDrain {
public:
    amDrain(ucp_worker_h worker, std::chrono::milliseconds timeout)
        : worker_(worker),
          timeout_(timeout) {
        if (timeout.count() < 0) {
            throw std::invalid_argument("UCX notification drain timeout must be nonnegative");
        }
    }

    [[nodiscard]] std::chrono::milliseconds
    timeout() const noexcept {
        return timeout_;
    }

    // Wait until the target has no pending sends or the deadline passes. With
    // progress, progress the worker meanwhile.
    [[nodiscard]] bool
    wait(const amSendStats &target,
         std::chrono::steady_clock::time_point deadline,
         bool progress) const {
        deadline = std::min(deadline, shutdownDeadline_);
        while (target.pending() != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            // progressLoop() can run indefinitely under sustained activity.
            // Check the deadline between individual UCX progress calls instead.
            if (!progress || (ucp_worker_progress(worker_) == 0)) {
                std::this_thread::yield();
            }
        }
        return true;
    }

    // Once progress threads have stopped: one teardown budget for the worker
    // and all its endpoints
    void
    beginShutdown(std::chrono::steady_clock::time_point deadline) noexcept {
        shutdownDeadline_ = std::min(shutdownDeadline_, deadline);
    }

private:
    const ucp_worker_h worker_;
    const std::chrono::milliseconds timeout_;
    std::chrono::steady_clock::time_point shutdownDeadline_ =
        std::chrono::steady_clock::time_point::max();
};

} // namespace nixl::ucx

#endif
