#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace fastfix::runtime {

inline constexpr std::uint64_t kDefaultTimerWheelTickNs = 1'000'000ULL;
inline constexpr std::size_t kDefaultTimerWheelSlotCount = 4096U;
inline constexpr std::uint64_t kMinimumTimerWheelTickNs = 1ULL;
inline constexpr std::size_t kMinimumTimerWheelSlotCount = 1U;

struct TimerWheelOptions {
    std::uint64_t tick_ns{kDefaultTimerWheelTickNs};
    std::size_t slot_count{kDefaultTimerWheelSlotCount};
};

class TimerWheel {
  public:
    explicit TimerWheel(TimerWheelOptions options = {})
        : options_{
                            .tick_ns = options.tick_ns == 0U ? kMinimumTimerWheelTickNs : options.tick_ns,
                            .slot_count = options.slot_count == 0U ? kMinimumTimerWheelSlotCount : options.slot_count,
          },
          slots_(options_.slot_count) {
    }

    auto Clear() -> void {
        initialized_ = false;
        base_time_ns_ = 0U;
        current_time_ns_ = 0U;
        current_tick_ = 0U;
        timers_.clear();
        immediate_timers_.clear();
        for (auto& slot : slots_) {
            slot.clear();
        }
        deadline_heap_.clear();
    }

    auto Schedule(std::uint64_t timer_id, std::uint64_t deadline_ns, std::uint64_t now_ns) -> void {
        EnsureInitialized(now_ns);

        auto it = timers_.find(timer_id);
        if (it != timers_.end()) {
            deadline_heap_.erase({it->second.deadline_ns, timer_id});
            RemoveTimer(timer_id, it->second);
        } else {
            it = timers_.emplace(timer_id, TimerRecord{}).first;
        }

        auto& record = it->second;
        record.deadline_ns = deadline_ns;
        if (deadline_ns <= current_time_ns_) {
            record.immediate = true;
            record.due_tick = current_tick_;
            record.slot_index = 0U;
            record.slot_position = immediate_timers_.size();
            immediate_timers_.push_back(timer_id);
        } else {
            record.immediate = false;
            record.due_tick = DueTick(deadline_ns);
            record.slot_index = static_cast<std::size_t>(record.due_tick % slots_.size());
            record.slot_position = slots_[record.slot_index].size();
            slots_[record.slot_index].push_back(timer_id);
        }

        deadline_heap_.insert({deadline_ns, timer_id});
    }

    auto Cancel(std::uint64_t timer_id) -> void {
        const auto it = timers_.find(timer_id);
        if (it == timers_.end()) {
            return;
        }

        deadline_heap_.erase({it->second.deadline_ns, timer_id});
        RemoveTimer(timer_id, it->second);
        timers_.erase(it);
    }

    auto PopExpired(std::uint64_t now_ns, std::vector<std::uint64_t>* expired_timer_ids) -> void {
        if (expired_timer_ids == nullptr) {
            return;
        }

        EnsureInitialized(now_ns);

        if (!immediate_timers_.empty()) {
            auto immediate = std::move(immediate_timers_);
            immediate_timers_.clear();
            for (const auto tid : immediate) {
                const auto it = timers_.find(tid);
                if (it == timers_.end() || !it->second.immediate) {
                    continue;
                }
                deadline_heap_.erase({it->second.deadline_ns, tid});
                expired_timer_ids->push_back(tid);
                timers_.erase(it);
            }
        }

        const auto target_tick = CurrentTick(now_ns);
        while (current_tick_ < target_tick) {
            ++current_tick_;
            auto& slot = slots_[current_tick_ % slots_.size()];
            if (slot.empty()) {
                continue;
            }

            auto pending = std::move(slot);
            slot.clear();
            for (const auto tid : pending) {
                const auto it = timers_.find(tid);
                if (it == timers_.end() || it->second.immediate) {
                    continue;
                }

                auto& record = it->second;
                if (record.due_tick <= current_tick_) {
                    deadline_heap_.erase({record.deadline_ns, tid});
                    expired_timer_ids->push_back(tid);
                    timers_.erase(it);
                    continue;
                }

                record.slot_position = slot.size();
                slot.push_back(tid);
            }
        }
    }

    [[nodiscard]] auto NextDeadline() const -> std::optional<std::uint64_t> {
        if (deadline_heap_.empty()) {
            return std::nullopt;
        }
        return deadline_heap_.begin()->first;
    }

  private:
    struct TimerRecord {
        std::uint64_t deadline_ns{0U};
        std::uint64_t due_tick{0U};
        std::size_t slot_index{0U};
        std::size_t slot_position{0U};
        bool immediate{false};
    };

    auto EnsureInitialized(std::uint64_t now_ns) -> void {
        if (!initialized_) {
            initialized_ = true;
            base_time_ns_ = now_ns;
            current_tick_ = 0U;
        }
        current_time_ns_ = now_ns;
    }

    [[nodiscard]] auto CurrentTick(std::uint64_t now_ns) const -> std::uint64_t {
        if (now_ns <= base_time_ns_) {
            return 0U;
        }
        return (now_ns - base_time_ns_) / options_.tick_ns;
    }

    [[nodiscard]] auto DueTick(std::uint64_t deadline_ns) const -> std::uint64_t {
        if (deadline_ns <= base_time_ns_) {
            return 0U;
        }

        const auto delta_ns = deadline_ns - base_time_ns_;
        return (delta_ns + options_.tick_ns - 1U) / options_.tick_ns;
    }

    auto RemoveTimer(std::uint64_t timer_id, TimerRecord& record) -> void {
        auto& container = record.immediate ? immediate_timers_ : slots_[record.slot_index];
        if (container.empty()) {
            return;
        }

        const auto last_position = container.size() - 1U;
        if (record.slot_position != last_position) {
            const auto moved_timer_id = container.back();
            container[record.slot_position] = moved_timer_id;
            auto moved = timers_.find(moved_timer_id);
            if (moved != timers_.end()) {
                moved->second.slot_position = record.slot_position;
                if (!record.immediate) {
                    moved->second.slot_index = record.slot_index;
                }
            }
        }
        container.pop_back();
    }

    TimerWheelOptions options_{};
    bool initialized_{false};
    std::uint64_t base_time_ns_{0U};
    std::uint64_t current_time_ns_{0U};
    std::uint64_t current_tick_{0U};
    std::unordered_map<std::uint64_t, TimerRecord> timers_;
    std::vector<std::uint64_t> immediate_timers_;
    std::vector<std::vector<std::uint64_t>> slots_;
    // Min-heap tracking the earliest deadline in O(log N) per update.
    std::set<std::pair<std::uint64_t, std::uint64_t>> deadline_heap_;
};

}  // namespace fastfix::runtime