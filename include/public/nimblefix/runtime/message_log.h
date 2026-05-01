#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/store/session_store.h"

namespace nimble::runtime {

/// Direction of a recorded FIX message in a message log.
enum class MessageDirection : std::uint8_t
{
  kInbound = 0,
  kOutbound = 1,
};

/// One recorded FIX message in a session message log.
struct MessageLogEntry
{
  std::uint64_t session_id{ 0 };
  std::uint32_t seq_num{ 0 };
  std::uint64_t timestamp_ns{ 0 };
  MessageDirection direction{ MessageDirection::kInbound };
  std::uint16_t flags{ 0 };
  std::vector<std::byte> payload;
};

/// A complete session message log containing inbound and outbound messages.
///
/// Entries are stored in timestamp order (mixed inbound/outbound).
struct MessageLog
{
  std::uint64_t session_id{ 0 };
  std::vector<MessageLogEntry> entries;

  /// Sort entries by timestamp, then by seq_num for ties.
  auto SortByTimestamp() -> void;

  /// Return only inbound entries.
  [[nodiscard]] auto InboundEntries() const -> std::vector<const MessageLogEntry*>;

  /// Return only outbound entries.
  [[nodiscard]] auto OutboundEntries() const -> std::vector<const MessageLogEntry*>;
};

// ─── Export from Store ─────────────────────────────────────────────────────

/// Export a complete message log for one session from a store.
///
/// Loads both outbound and inbound messages in the given sequence range
/// and merges them by timestamp.
///
/// \param store Session store to read from.
/// \param session_id Runtime session id.
/// \param outbound_begin First outbound sequence number (inclusive), default 1.
/// \param outbound_end Last outbound sequence number (inclusive), 0 = max available.
/// \return MessageLog on success.
[[nodiscard]] auto
ExportMessageLog(const store::SessionStore& store,
                 std::uint64_t session_id,
                 std::uint32_t outbound_begin = 1,
                 std::uint32_t outbound_end = 0) -> base::Result<MessageLog>;

// ─── Serialization ─────────────────────────────────────────────────────────

/// Serialize a message log to JSON string.
[[nodiscard]] auto
MessageLogToJson(const MessageLog& log) -> std::string;

/// Deserialize a message log from JSON string.
[[nodiscard]] auto
MessageLogFromJson(std::string_view json) -> base::Result<MessageLog>;

/// Write a message log to a file in JSON format.
[[nodiscard]] auto
WriteMessageLog(const MessageLog& log, const std::filesystem::path& path) -> base::Status;

/// Read a message log from a JSON file.
[[nodiscard]] auto
ReadMessageLog(const std::filesystem::path& path) -> base::Result<MessageLog>;

// ─── Deterministic Replay ──────────────────────────────────────────────────

/// Replay speed mode for the replay controller.
enum class ReplaySpeed : std::uint32_t
{
  /// Process all messages as fast as possible.
  kMaxSpeed = 0,
  /// Delay between messages proportional to original timestamp gaps.
  kRealTime,
  /// Advance one message at a time via Step().
  kStep,
};

/// Deterministic replay controller that feeds recorded messages through a callback.
///
/// Typical usage:
///   ReplayController controller;
///   controller.LoadLog(std::move(log));
///   controller.SetCallback([](const MessageLogEntry& entry) {
///     // process entry
///     return base::Status::Ok();
///   });
///   controller.Run();  // blocks until all entries processed
class ReplayController
{
public:
  using ReplayCallback = std::function<base::Status(const MessageLogEntry& entry)>;

  /// Load a message log for replay. Resets position to 0.
  auto LoadLog(MessageLog log) -> void;

  /// Set the replay speed mode.
  auto SetSpeed(ReplaySpeed speed) -> void;

  /// Set the callback invoked for each replayed message.
  auto SetCallback(ReplayCallback callback) -> void;

  /// Run the replay to completion (blocking). Not valid in kStep mode.
  /// Returns first error from callback, or Ok if all succeed.
  [[nodiscard]] auto Run() -> base::Status;

  /// Advance one message (valid in any mode). Returns the processed entry
  /// or nullptr if finished.
  [[nodiscard]] auto Step() -> base::Result<const MessageLogEntry*>;

  /// Reset replay position to the beginning.
  auto Reset() -> void;

  /// Current replay position (0-based index into entries).
  [[nodiscard]] auto position() const -> std::size_t;

  /// Total number of entries in the loaded log.
  [[nodiscard]] auto total_entries() const -> std::size_t;

  /// Whether all entries have been replayed.
  [[nodiscard]] auto finished() const -> bool;

private:
  MessageLog log_;
  ReplayCallback callback_;
  ReplaySpeed speed_{ ReplaySpeed::kMaxSpeed };
  std::size_t position_{ 0 };
};

} // namespace nimble::runtime
