#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/config.h"

namespace nimble::runtime {

class Engine;

// ─── Schedule Status ───────────────────────────────────────────────────────

/// Current status of a session's schedule windows.
struct SessionScheduleStatus
{
  std::uint64_t session_id{ 0 };
  /// Whether the session is within its configured session-active window.
  bool in_session_window{ true };
  /// Whether the session is within its configured logon-allowed window.
  bool in_logon_window{ true };
  /// Next time the session window opens (nullopt if already open or non_stop).
  std::optional<std::uint64_t> next_session_window_open_ns;
  /// Next time the logon window opens (nullopt if already open or no logon window).
  std::optional<std::uint64_t> next_logon_window_open_ns;
  /// Next time the session window closes (nullopt if no window configured).
  std::optional<std::uint64_t> next_session_window_close_ns;
  /// Whether the session is configured as non-stop.
  bool non_stop{ false };
};

/// Schedule-related event categories that diagnostics or applications can emit
/// when comparing successive SessionScheduleStatus snapshots.
enum class SessionScheduleEventKind : std::uint32_t
{
  kEnteredSessionWindow = 0,
  kExitedSessionWindow,
  kEnteredLogonWindow,
  kExitedLogonWindow,
  kBlackoutStarted,
  kBlackoutEnded,
  kDayCutTriggered,
};

/// A schedule event notification payload.
struct SessionScheduleEvent
{
  std::uint64_t session_id{ 0 };
  std::uint64_t unix_time_ns{ 0 };
  SessionScheduleEventKind kind{ SessionScheduleEventKind::kEnteredSessionWindow };
  SessionScheduleStatus status;
};

/// Query the schedule status for a specific session at a given wall-clock time.
///
/// \param config Counterparty config containing the session_schedule.
/// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
/// \return Populated SessionScheduleStatus.
[[nodiscard]] auto
QueryScheduleStatus(const CounterpartyConfig& config, std::uint64_t unix_time_ns) -> SessionScheduleStatus;

// ─── Window Close Time ─────────────────────────────────────────────────────

/// Find the next time the session-active window closes.
///
/// Boundary condition: when no session window is configured, returns nullopt.
///
/// \param schedule Schedule to evaluate.
/// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
/// \return Next closing timestamp, or nullopt when no window is configured.
[[nodiscard]] auto
NextSessionWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>;

/// Find the next time the logon window closes.
[[nodiscard]] auto
NextLogonWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>;

/// Find the next time the session-active window opens.
/// (Complement to the existing NextLogonWindowStart in config.h)
[[nodiscard]] auto
NextSessionWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>;

// ─── Holiday / Blackout Calendar ───────────────────────────────────────────

/// A single blackout date (YYYYMMDD as integer, e.g. 20260704).
using BlackoutDate = std::uint32_t;

/// Holiday/blackout calendar for session scheduling.
///
/// When a blackout calendar is set for a counterparty, the session is treated
/// as outside its session window on blackout dates regardless of the normal
/// schedule. Logon is also blocked on blackout dates.
///
/// Date format: YYYYMMDD as uint32_t (e.g., 20260704 for July 4, 2026).
class BlackoutCalendar
{
public:
  BlackoutCalendar() = default;

  /// Add a single blackout date.
  auto AddDate(BlackoutDate date) -> void;

  /// Add multiple blackout dates.
  auto AddDates(std::vector<BlackoutDate> dates) -> void;

  /// Remove a blackout date. No-op if not present.
  auto RemoveDate(BlackoutDate date) -> void;

  /// Clear all blackout dates.
  auto Clear() -> void;

  /// Check whether a given nanosecond timestamp falls on a blackout date.
  ///
  /// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
  /// \param use_local_time If true, convert to local time before extracting date.
  /// \return True if the date is in the blackout set.
  [[nodiscard]] auto IsBlackout(std::uint64_t unix_time_ns, bool use_local_time = false) const -> bool;

  /// Check whether a given YYYYMMDD date is in the blackout set.
  [[nodiscard]] auto IsBlackoutDate(BlackoutDate date) const -> bool;

  /// Return all blackout dates, sorted ascending.
  [[nodiscard]] auto dates() const -> std::vector<BlackoutDate>;

  /// Return the number of blackout dates.
  [[nodiscard]] auto size() const -> std::size_t;

  /// Whether the calendar is empty (no blackout dates).
  [[nodiscard]] auto empty() const -> bool;

private:
  std::vector<BlackoutDate> dates_;
};

/// Convert a YYYYMMDD integer to a human-readable string (YYYY-MM-DD).
[[nodiscard]] auto
BlackoutDateToString(BlackoutDate date) -> std::string;

/// Parse a human-readable date string (YYYY-MM-DD or YYYYMMDD) to BlackoutDate.
[[nodiscard]] auto
ParseBlackoutDate(std::string_view text) -> base::Result<BlackoutDate>;

/// Extract the YYYYMMDD date from a nanosecond timestamp.
[[nodiscard]] auto
ExtractDate(std::uint64_t unix_time_ns, bool use_local_time = false) -> BlackoutDate;

// ─── Schedule-Aware Window Checks ──────────────────────────────────────────

/// Like IsWithinSessionWindow but also considers a blackout calendar.
///
/// Returns false if the timestamp falls on a blackout date.
[[nodiscard]] auto
IsWithinSessionWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                   const BlackoutCalendar& calendar,
                                   std::uint64_t unix_time_ns) -> bool;

/// Like IsWithinLogonWindow but also considers a blackout calendar.
[[nodiscard]] auto
IsWithinLogonWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                 const BlackoutCalendar& calendar,
                                 std::uint64_t unix_time_ns) -> bool;

// ─── Day-Cut Trigger API ───────────────────────────────────────────────────

/// Trigger an explicit day-cut sequence reset for one session.
///
/// This is the public API for DayCutMode::kExternalControl and can also be
/// used to force a reset regardless of the configured mode.
///
/// The session must be in kDisconnected or kActive state.
/// Sequence numbers are reset to 1 for both inbound and outbound.
///
/// Note: this control-plane API validates that the session exists but does not
/// directly mutate SessionCore from another thread. Runtime resets still happen
/// through SessionCore::CheckDayCut on worker-owned state; callers that need an
/// immediate externally controlled cut should coordinate disconnect/reconnect
/// sequencing and ResetSeqNumFlag at the application layer.
///
/// \param engine Booted engine.
/// \param session_id Session to reset.
/// \return Ok on success, NotFound if session_id unknown.
[[nodiscard]] auto
TriggerDayCut(Engine& engine, std::uint64_t session_id) -> base::Status;

} // namespace nimble::runtime
