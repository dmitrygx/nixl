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

#ifndef NIXL_SRC_PLUGINS_UCX_AM_DRAIN_H
#define NIXL_SRC_PLUGINS_UCX_AM_DRAIN_H

#include <chrono>
#include <thread>

extern "C" {
#include <ucp/api/ucp.h>
}

#include "am_send.h"

namespace nixl::ucx {

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

// Progress the worker until the sends have none pending or the deadline
// passes. Return whether none is pending.
[[nodiscard]] inline bool
amDrainWait(ucp_worker_h worker,
            const amSendStats &sends,
            std::chrono::steady_clock::time_point deadline) {
    while (sends.pending() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        // progressLoop() can run indefinitely under sustained activity.
        // Check the deadline between individual UCX progress calls instead.
        if (ucp_worker_progress(worker) == 0) {
            std::this_thread::yield();
        }
    }
    return true;
}

} // namespace nixl::ucx

#endif
