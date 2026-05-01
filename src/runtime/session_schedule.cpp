#include "nimblefix/runtime/session_schedule.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/schedule_helpers.h"

namespace nimble::runtime {

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint32_t kMonthsPerYear = 12U;
constexpr std::uint32_t kMaxDaysPerMonth = 31U;
constexpr std::size_t kCompactDateLength = 8U;
constexpr std::size_t kDashedDateLength = 10U;

auto
WindowCycleSeconds(const detail::SessionWindowSpec& window) -> int
{
  return window.start_day.has_value() ? 7 * detail::kSecondsPerDay : detail::kSecondsPerDay;
}

auto
CurrentWindowStartSecond(const detail::SessionWindowSpec& window, const detail::CalendarPoint& point) -> int
{
  const auto current =
    window.start_day.has_value() ? point.weekday * detail::kSecondsPerDay + point.second_of_day : point.second_of_day;
  const auto start = window.start_day.has_value() ? *window.start_day * detail::kSecondsPerDay + window.start_second
                                                  : window.start_second;
  const auto end =
    window.end_day.has_value() ? *window.end_day * detail::kSecondsPerDay + window.end_second : window.end_second;
  const auto cycle = WindowCycleSeconds(window);

  if (start == end) {
    return current;
  }
  if (start < end) {
    return start;
  }
  return current >= start ? start : start - cycle;
}

auto
NextWindowClose(const detail::SessionWindowSpec& window, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  if (!detail::IsWithinWindow(window, unix_time_ns)) {
    return std::nullopt;
  }

  const auto point = detail::BuildCalendarPoint(unix_time_ns, window.use_local_time);
  const auto start = CurrentWindowStartSecond(window, point);
  const auto duration = [&window]() {
    const auto cycle = WindowCycleSeconds(window);
    const auto raw_end =
      window.end_day.has_value() ? *window.end_day * detail::kSecondsPerDay + window.end_second : window.end_second;
    const auto raw_start = window.start_day.has_value()
                             ? *window.start_day * detail::kSecondsPerDay + window.start_second
                             : window.start_second;
    auto span = raw_end - raw_start;
    if (span <= 0) {
      span += cycle;
    }
    return span;
  }();
  const auto close_second = start + duration;

  auto candidate = point.civil_time;
  const auto current_base = window.start_day.has_value() ? point.weekday * detail::kSecondsPerDay : 0;
  const auto delta_seconds = close_second - (current_base + point.second_of_day);
  candidate.tm_sec += delta_seconds;
  return detail::MakeUnixTimeNs(candidate, window.use_local_time);
}

auto
IsLeapYear(std::uint32_t year) -> bool
{
  return (year % 4U == 0U && year % 100U != 0U) || (year % 400U == 0U);
}

auto
DaysInMonth(std::uint32_t year, std::uint32_t month) -> std::uint32_t
{
  static constexpr std::array<std::uint32_t, kMonthsPerYear> kDaysByMonth{ 31U, 28U, 31U, 30U, 31U, 30U,
                                                                           31U, 31U, 30U, 31U, 30U, 31U };
  if (month == 0U || month > kMonthsPerYear) {
    return 0U;
  }
  if (month == 2U && IsLeapYear(year)) {
    return 29U;
  }
  return kDaysByMonth[month - 1U];
}

auto
ValidateDateParts(std::uint32_t year, std::uint32_t month, std::uint32_t day) -> bool
{
  if (month == 0U || month > kMonthsPerYear || day == 0U || day > kMaxDaysPerMonth) {
    return false;
  }
  return day <= DaysInMonth(year, month);
}

auto
ParseDigits(std::string_view text, std::uint32_t& value) -> bool
{
  if (text.empty()) {
    return false;
  }
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  return ec == std::errc{} && ptr == last;
}

auto
DateParts(BlackoutDate date) -> std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
{
  const auto year = date / 10000U;
  const auto month = (date / 100U) % 100U;
  const auto day = date % 100U;
  return { year, month, day };
}

} // namespace

auto
QueryScheduleStatus(const CounterpartyConfig& config, std::uint64_t unix_time_ns) -> SessionScheduleStatus
{
  SessionScheduleStatus status;
  status.session_id = config.session.session_id;
  if (config.session_schedule.non_stop_session) {
    status.in_session_window = true;
    status.in_logon_window = true;
    status.non_stop = true;
    return status;
  }

  status.in_session_window = IsWithinSessionWindow(config.session_schedule, unix_time_ns);
  status.in_logon_window = IsWithinLogonWindow(config.session_schedule, unix_time_ns);
  if (!status.in_logon_window) {
    status.next_logon_window_open_ns = NextLogonWindowStart(config.session_schedule, unix_time_ns);
  }
  if (!status.in_session_window) {
    status.next_session_window_open_ns = NextSessionWindowStart(config.session_schedule, unix_time_ns);
  }
  if (status.in_session_window) {
    status.next_session_window_close_ns = NextSessionWindowClose(config.session_schedule, unix_time_ns);
  }
  return status;
}

auto
NextSessionWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  const auto window = detail::BuildWindowSpec(schedule, false);
  if (!window.has_value()) {
    return std::nullopt;
  }
  return NextWindowClose(*window, unix_time_ns);
}

auto
NextLogonWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  const auto window = detail::BuildWindowSpec(schedule, true);
  if (!window.has_value()) {
    return std::nullopt;
  }
  return NextWindowClose(*window, unix_time_ns);
}

auto
NextSessionWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  const auto window = detail::BuildWindowSpec(schedule, false);
  if (!window.has_value()) {
    return unix_time_ns;
  }
  return detail::NextWindowStart(*window, unix_time_ns);
}

auto
BlackoutCalendar::AddDate(BlackoutDate date) -> void
{
  const auto it = std::lower_bound(dates_.begin(), dates_.end(), date);
  if (it == dates_.end() || *it != date) {
    dates_.insert(it, date);
  }
}

auto
BlackoutCalendar::AddDates(std::vector<BlackoutDate> dates) -> void
{
  for (const auto date : dates) {
    AddDate(date);
  }
}

auto
BlackoutCalendar::RemoveDate(BlackoutDate date) -> void
{
  const auto it = std::lower_bound(dates_.begin(), dates_.end(), date);
  if (it != dates_.end() && *it == date) {
    dates_.erase(it);
  }
}

auto
BlackoutCalendar::Clear() -> void
{
  dates_.clear();
}

auto
BlackoutCalendar::IsBlackout(std::uint64_t unix_time_ns, bool use_local_time) const -> bool
{
  return IsBlackoutDate(ExtractDate(unix_time_ns, use_local_time));
}

auto
BlackoutCalendar::IsBlackoutDate(BlackoutDate date) const -> bool
{
  return std::binary_search(dates_.begin(), dates_.end(), date);
}

auto
BlackoutCalendar::dates() const -> std::vector<BlackoutDate>
{
  return dates_;
}

auto
BlackoutCalendar::size() const -> std::size_t
{
  return dates_.size();
}

auto
BlackoutCalendar::empty() const -> bool
{
  return dates_.empty();
}

auto
BlackoutDateToString(BlackoutDate date) -> std::string
{
  const auto [year, month, day] = DateParts(date);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month << '-' << std::setw(2) << day;
  return out.str();
}

auto
ParseBlackoutDate(std::string_view text) -> base::Result<BlackoutDate>
{
  std::uint32_t year = 0U;
  std::uint32_t month = 0U;
  std::uint32_t day = 0U;

  if (text.size() == kDashedDateLength && text[4] == '-' && text[7] == '-') {
    if (!ParseDigits(text.substr(0, 4), year) || !ParseDigits(text.substr(5, 2), month) ||
        !ParseDigits(text.substr(8, 2), day)) {
      return base::Status::InvalidArgument("blackout date must use YYYY-MM-DD digits");
    }
  } else if (text.size() == kCompactDateLength) {
    if (!ParseDigits(text.substr(0, 4), year) || !ParseDigits(text.substr(4, 2), month) ||
        !ParseDigits(text.substr(6, 2), day)) {
      return base::Status::InvalidArgument("blackout date must use YYYYMMDD digits");
    }
  } else {
    return base::Status::InvalidArgument("blackout date must be YYYY-MM-DD or YYYYMMDD");
  }

  if (!ValidateDateParts(year, month, day)) {
    return base::Status::InvalidArgument("blackout date is out of range");
  }
  return year * 10000U + month * 100U + day;
}

auto
ExtractDate(std::uint64_t unix_time_ns, bool use_local_time) -> BlackoutDate
{
  const auto unix_seconds = static_cast<std::time_t>(unix_time_ns / kNanosecondsPerSecond);
  std::tm civil_time{};
  if (use_local_time) {
    localtime_r(&unix_seconds, &civil_time);
  } else {
    gmtime_r(&unix_seconds, &civil_time);
  }
  return static_cast<BlackoutDate>((civil_time.tm_year + 1900) * 10000 + (civil_time.tm_mon + 1) * 100 +
                                   civil_time.tm_mday);
}

auto
IsWithinSessionWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                   const BlackoutCalendar& calendar,
                                   std::uint64_t unix_time_ns) -> bool
{
  if (calendar.IsBlackout(unix_time_ns, schedule.use_local_time)) {
    return false;
  }
  return IsWithinSessionWindow(schedule, unix_time_ns);
}

auto
IsWithinLogonWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                 const BlackoutCalendar& calendar,
                                 std::uint64_t unix_time_ns) -> bool
{
  if (calendar.IsBlackout(unix_time_ns, schedule.use_local_time)) {
    return false;
  }
  return IsWithinLogonWindow(schedule, unix_time_ns);
}

auto
TriggerDayCut(Engine& engine, std::uint64_t session_id) -> base::Status
{
  if (engine.FindCounterpartyConfig(session_id) == nullptr) {
    return base::Status::NotFound("counterparty session not found");
  }
  return base::Status::Ok();
}

} // namespace nimble::runtime
