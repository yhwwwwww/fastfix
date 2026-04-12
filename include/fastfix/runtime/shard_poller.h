#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "fastfix/base/status.h"
#include "fastfix/runtime/io_poller.h"
#include "fastfix/runtime/poll_wakeup.h"
#include "fastfix/runtime/timer_wheel.h"

namespace fastfix::runtime {

class ShardPoller {
  public:
    struct IoReadyState {
        bool wakeup_signaled{false};
        std::vector<std::size_t> ready_indices;
    };

    auto OpenWakeup() -> base::Status;
    auto CloseWakeup() -> void;
    auto SignalWakeup() const -> void;
    auto DrainWakeup() -> void;

    /// Initialize an IoPoller backend (epoll or io_uring).
    auto InitBackend(IoBackend backend) -> base::Status;

    /// Returns true if using IoPoller backend (epoll or io_uring).
    [[nodiscard]] auto has_io_poller() const -> bool { return io_poller_ != nullptr; }
    [[nodiscard]] auto io_poller() const -> IoPoller* { return io_poller_.get(); }

    auto ClearTimers() -> void {
        timer_wheel_.Clear();
    }

    [[nodiscard]] auto NextDeadline() const -> std::optional<std::uint64_t> {
        return timer_wheel_.NextDeadline();
    }

    [[nodiscard]] auto timer_wheel() -> TimerWheel& {
        return timer_wheel_;
    }

    [[nodiscard]] auto timer_wheel() const -> const TimerWheel& {
        return timer_wheel_;
    }

    template <typename ConnectionFdProvider>
    auto SyncAndWait(
        std::size_t connection_count,
        ConnectionFdProvider&& connection_fd_provider,
        std::chrono::milliseconds timeout,
        IoReadyState& out) -> base::Status {
        out.wakeup_signaled = false;
        out.ready_indices.clear();

        // Build current fd set and fd→index map.
        fd_to_index_.clear();
        fd_to_index_.reserve(connection_count);
        current_fds_.clear();
        current_fds_.reserve(connection_count);
        for (std::size_t i = 0; i < connection_count; ++i) {
            const int fd = connection_fd_provider(i);
            if (fd >= 0) {
                current_fds_.insert(fd);
                fd_to_index_[fd] = i;
            }
        }

        // Remove stale fds.
        for (auto it = registered_fds_.begin(); it != registered_fds_.end(); ) {
            if (!current_fds_.contains(*it)) {
                io_poller_->RemoveFd(*it);
                it = registered_fds_.erase(it);
            } else {
                ++it;
            }
        }

        // Add new fds (use fd value as tag).
        for (const int fd : current_fds_) {
            if (!registered_fds_.contains(fd)) {
                auto status = io_poller_->AddFd(fd, static_cast<std::size_t>(fd));
                if (!status.ok()) return status;
                registered_fds_.insert(fd);
            }
        }

        // Wait for events.
        auto result = io_poller_->Wait(timeout);
        if (!result.ok()) return result.status();
        const int ready_count = result.value();

        // Drain wakeup if signaled and map ready tags to connection indices.
        for (int i = 0; i < ready_count; ++i) {
            const auto tag = io_poller_->ReadyTag(i);
            if (tag == kWakeupTag) {
                out.wakeup_signaled = true;
                continue;
            }
            const auto fd = static_cast<int>(tag);
            auto it = fd_to_index_.find(fd);
            if (it != fd_to_index_.end()) {
                out.ready_indices.push_back(it->second);
            }
        }

        if (out.wakeup_signaled) {
            DrainWakeup();
        }

        return base::Status::Ok();
    }

  private:
    static constexpr std::size_t kWakeupTag = ~std::size_t{0};

    TimerWheel timer_wheel_{};
    PollWakeup wakeup_{};
    std::unique_ptr<IoPoller> io_poller_;
    std::unordered_set<int> registered_fds_;
    // Reused per SyncAndWait call to avoid hot-path heap allocation.
    std::unordered_map<int, std::size_t> fd_to_index_;
    std::unordered_set<int> current_fds_;
};

}  // namespace fastfix::runtime