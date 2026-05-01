#include "nimblefix/runtime/message_log.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace nimble::runtime {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] auto
DirectionToString(MessageDirection direction) -> std::string_view
{
  switch (direction) {
    case MessageDirection::kInbound:
      return "inbound";
    case MessageDirection::kOutbound:
      return "outbound";
  }
  return "inbound";
}

[[nodiscard]] auto
DirectionSortValue(MessageDirection direction) -> std::uint8_t
{
  return static_cast<std::uint8_t>(direction);
}

[[nodiscard]] auto
RecordToEntry(const store::MessageRecord& record, MessageDirection direction) -> MessageLogEntry
{
  return MessageLogEntry{
    .session_id = record.session_id,
    .seq_num = record.seq_num,
    .timestamp_ns = record.timestamp_ns,
    .direction = direction,
    .flags = record.flags,
    .payload = record.payload,
  };
}

auto
AppendJsonString(std::string& out, std::string_view value) -> void
{
  out.push_back('"');
  for (const auto raw_ch : value) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (ch < 0x20U) {
          out.append("\\u00");
          out.push_back(kHexDigits[(ch >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

auto
AppendHexPayload(std::string& out, const std::vector<std::byte>& payload) -> void
{
  out.push_back('"');
  for (const auto byte : payload) {
    const auto value = static_cast<unsigned int>(std::to_integer<unsigned char>(byte));
    out.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
    out.push_back(kHexDigits[value & 0x0FU]);
  }
  out.push_back('"');
}

[[nodiscard]] auto
HexValue(char ch) -> int
{
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] auto
DecodeHexPayload(std::string_view hex) -> base::Result<std::vector<std::byte>>
{
  if ((hex.size() % 2U) != 0U) {
    return base::Status::FormatError("message log payload hex has odd length");
  }

  std::vector<std::byte> payload;
  payload.reserve(hex.size() / 2U);
  for (std::size_t index = 0U; index < hex.size(); index += 2U) {
    const auto high = HexValue(hex[index]);
    const auto low = HexValue(hex[index + 1U]);
    if (high < 0 || low < 0) {
      return base::Status::FormatError("message log payload contains non-hex data");
    }
    payload.push_back(static_cast<std::byte>((high << 4U) | low));
  }
  return payload;
}

class MessageLogJsonParser
{
public:
  explicit MessageLogJsonParser(std::string_view input)
    : input_(input)
  {
  }

  auto Parse() -> base::Result<MessageLog>
  {
    MessageLog log;
    auto status = Expect('{');
    if (!status.ok()) {
      return status;
    }

    bool have_session_id = false;
    bool have_entries = false;
    while (true) {
      if (Consume('}')) {
        break;
      }

      auto key = ParseString();
      if (!key.ok()) {
        return key.status();
      }
      status = Expect(':');
      if (!status.ok()) {
        return status;
      }

      if (key.value() == "session_id") {
        auto value = ParseUnsigned<std::uint64_t>();
        if (!value.ok()) {
          return value.status();
        }
        log.session_id = value.value();
        have_session_id = true;
      } else if (key.value() == "entries") {
        auto entries = ParseEntries();
        if (!entries.ok()) {
          return entries.status();
        }
        log.entries = std::move(entries).value();
        have_entries = true;
      } else {
        return base::Status::FormatError("message log has an unexpected top-level field");
      }

      if (Consume(',')) {
        continue;
      }
      status = Expect('}');
      if (!status.ok()) {
        return status;
      }
      break;
    }

    SkipWhitespace();
    if (position_ != input_.size()) {
      return base::Status::FormatError("message log JSON has trailing data");
    }
    if (!have_session_id || !have_entries) {
      return base::Status::FormatError("message log JSON is missing required fields");
    }
    for (const auto& entry : log.entries) {
      if (entry.session_id != log.session_id) {
        return base::Status::FormatError("message log entry session_id does not match log session_id");
      }
    }
    return log;
  }

private:
  auto ParseEntries() -> base::Result<std::vector<MessageLogEntry>>
  {
    std::vector<MessageLogEntry> entries;
    auto status = Expect('[');
    if (!status.ok()) {
      return status;
    }
    while (true) {
      if (Consume(']')) {
        break;
      }
      auto entry = ParseEntry();
      if (!entry.ok()) {
        return entry.status();
      }
      entries.push_back(std::move(entry).value());
      if (Consume(',')) {
        continue;
      }
      status = Expect(']');
      if (!status.ok()) {
        return status;
      }
      break;
    }
    return entries;
  }

  auto ParseEntry() -> base::Result<MessageLogEntry>
  {
    MessageLogEntry entry;
    auto status = Expect('{');
    if (!status.ok()) {
      return status;
    }

    bool have_session_id = false;
    bool have_seq_num = false;
    bool have_timestamp = false;
    bool have_direction = false;
    bool have_flags = false;
    bool have_payload = false;

    while (true) {
      if (Consume('}')) {
        break;
      }

      auto key = ParseString();
      if (!key.ok()) {
        return key.status();
      }
      status = Expect(':');
      if (!status.ok()) {
        return status;
      }

      if (key.value() == "session_id") {
        auto value = ParseUnsigned<std::uint64_t>();
        if (!value.ok()) {
          return value.status();
        }
        entry.session_id = value.value();
        have_session_id = true;
      } else if (key.value() == "seq_num") {
        auto value = ParseUnsigned<std::uint32_t>();
        if (!value.ok()) {
          return value.status();
        }
        entry.seq_num = value.value();
        have_seq_num = true;
      } else if (key.value() == "timestamp_ns") {
        auto value = ParseUnsigned<std::uint64_t>();
        if (!value.ok()) {
          return value.status();
        }
        entry.timestamp_ns = value.value();
        have_timestamp = true;
      } else if (key.value() == "direction") {
        auto value = ParseString();
        if (!value.ok()) {
          return value.status();
        }
        if (value.value() == "inbound") {
          entry.direction = MessageDirection::kInbound;
        } else if (value.value() == "outbound") {
          entry.direction = MessageDirection::kOutbound;
        } else {
          return base::Status::FormatError("message log entry has an invalid direction");
        }
        have_direction = true;
      } else if (key.value() == "flags") {
        auto value = ParseUnsigned<std::uint16_t>();
        if (!value.ok()) {
          return value.status();
        }
        entry.flags = value.value();
        have_flags = true;
      } else if (key.value() == "payload") {
        auto value = ParseString();
        if (!value.ok()) {
          return value.status();
        }
        auto payload = DecodeHexPayload(value.value());
        if (!payload.ok()) {
          return payload.status();
        }
        entry.payload = std::move(payload).value();
        have_payload = true;
      } else {
        return base::Status::FormatError("message log entry has an unexpected field");
      }

      if (Consume(',')) {
        continue;
      }
      status = Expect('}');
      if (!status.ok()) {
        return status;
      }
      break;
    }

    if (!have_session_id || !have_seq_num || !have_timestamp || !have_direction || !have_flags || !have_payload) {
      return base::Status::FormatError("message log entry is missing required fields");
    }
    return entry;
  }

  template<typename Value>
  auto ParseUnsigned() -> base::Result<Value>
  {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
      return base::Status::FormatError("expected unsigned integer in message log JSON");
    }
    const auto begin = input_.data() + position_;
    while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
      ++position_;
    }
    const auto end = input_.data() + position_;

    Value value{};
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc() || ptr != end) {
      return base::Status::FormatError("invalid unsigned integer in message log JSON");
    }
    return value;
  }

  auto ParseString() -> base::Result<std::string>
  {
    auto status = Expect('"');
    if (!status.ok()) {
      return status;
    }

    std::string value;
    while (position_ < input_.size()) {
      const char ch = input_[position_++];
      if (ch == '"') {
        return value;
      }
      if (ch == '\\') {
        if (position_ >= input_.size()) {
          return base::Status::FormatError("unterminated escape in message log JSON string");
        }
        const char escaped = input_[position_++];
        switch (escaped) {
          case '"':
            value.push_back('"');
            break;
          case '\\':
            value.push_back('\\');
            break;
          case '/':
            value.push_back('/');
            break;
          case 'b':
            value.push_back('\b');
            break;
          case 'f':
            value.push_back('\f');
            break;
          case 'n':
            value.push_back('\n');
            break;
          case 'r':
            value.push_back('\r');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'u': {
            if (position_ + 4U > input_.size()) {
              return base::Status::FormatError("truncated unicode escape in message log JSON string");
            }
            unsigned int code = 0U;
            for (std::size_t count = 0U; count < 4U; ++count) {
              const auto digit = HexValue(input_[position_++]);
              if (digit < 0) {
                return base::Status::FormatError("invalid unicode escape in message log JSON string");
              }
              code = (code << 4U) | static_cast<unsigned int>(digit);
            }
            if (code > 0x7FU) {
              return base::Status::FormatError("message log JSON parser only supports ASCII unicode escapes");
            }
            value.push_back(static_cast<char>(code));
            break;
          }
          default:
            return base::Status::FormatError("invalid escape in message log JSON string");
        }
      } else {
        if (static_cast<unsigned char>(ch) < 0x20U) {
          return base::Status::FormatError("control character in message log JSON string");
        }
        value.push_back(ch);
      }
    }

    return base::Status::FormatError("unterminated message log JSON string");
  }

  auto Expect(char expected) -> base::Status
  {
    SkipWhitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      std::string message = "expected '";
      message.push_back(expected);
      message.append("' in message log JSON");
      return base::Status::FormatError(std::move(message));
    }
    ++position_;
    return base::Status::Ok();
  }

  [[nodiscard]] auto Consume(char expected) -> bool
  {
    SkipWhitespace();
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  auto SkipWhitespace() -> void
  {
    while (position_ < input_.size()) {
      const auto ch = input_[position_];
      if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
        break;
      }
      ++position_;
    }
  }

  std::string_view input_;
  std::size_t position_{ 0U };
};

} // namespace

auto
MessageLog::SortByTimestamp() -> void
{
  std::stable_sort(entries.begin(), entries.end(), [](const MessageLogEntry& lhs, const MessageLogEntry& rhs) {
    if (lhs.timestamp_ns != rhs.timestamp_ns) {
      return lhs.timestamp_ns < rhs.timestamp_ns;
    }
    if (lhs.seq_num != rhs.seq_num) {
      return lhs.seq_num < rhs.seq_num;
    }
    return DirectionSortValue(lhs.direction) < DirectionSortValue(rhs.direction);
  });
}

auto
MessageLog::InboundEntries() const -> std::vector<const MessageLogEntry*>
{
  std::vector<const MessageLogEntry*> result;
  result.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.direction == MessageDirection::kInbound) {
      result.push_back(&entry);
    }
  }
  return result;
}

auto
MessageLog::OutboundEntries() const -> std::vector<const MessageLogEntry*>
{
  std::vector<const MessageLogEntry*> result;
  result.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.direction == MessageDirection::kOutbound) {
      result.push_back(&entry);
    }
  }
  return result;
}

auto
ExportMessageLog(const store::SessionStore& store,
                 std::uint64_t session_id,
                 std::uint32_t outbound_begin,
                 std::uint32_t outbound_end) -> base::Result<MessageLog>
{
  if (session_id == 0U) {
    return base::Status::InvalidArgument("message log export requires a valid session id");
  }
  if (outbound_begin == 0U) {
    return base::Status::InvalidArgument("message log export outbound_begin must be non-zero");
  }

  auto recovery = store.LoadRecoveryState(session_id);
  if (!recovery.ok()) {
    return recovery.status();
  }

  const auto& state = recovery.value();
  std::vector<store::MessageRecord> outbound_records;
  const std::uint32_t max_outbound_seq = state.next_out_seq == 0U ? 0U : state.next_out_seq - 1U;
  if (outbound_end == 0U) {
    outbound_end = max_outbound_seq;
  }
  if (outbound_end != 0U && outbound_begin <= outbound_end) {
    auto outbound = store.LoadOutboundRange(session_id, outbound_begin, outbound_end);
    if (!outbound.ok()) {
      return outbound.status();
    }
    outbound_records = std::move(outbound).value();
  }

  std::vector<store::MessageRecord> inbound_records;
  const std::uint32_t max_inbound_seq = state.next_in_seq == 0U ? 0U : state.next_in_seq - 1U;
  if (max_inbound_seq != 0U) {
    auto inbound = store.LoadInboundRange(session_id, 1U, max_inbound_seq);
    if (!inbound.ok()) {
      return inbound.status();
    }
    inbound_records = std::move(inbound).value();
  }

  MessageLog log;
  log.session_id = session_id;
  log.entries.reserve(outbound_records.size() + inbound_records.size());
  for (const auto& record : inbound_records) {
    log.entries.push_back(RecordToEntry(record, MessageDirection::kInbound));
  }
  for (const auto& record : outbound_records) {
    log.entries.push_back(RecordToEntry(record, MessageDirection::kOutbound));
  }
  log.SortByTimestamp();
  return log;
}

auto
MessageLogToJson(const MessageLog& log) -> std::string
{
  std::string out;
  out.reserve(64U + (log.entries.size() * 160U));
  out.append("{\n  \"session_id\": ");
  out.append(std::to_string(log.session_id));
  out.append(",\n  \"entries\": [");
  for (std::size_t index = 0U; index < log.entries.size(); ++index) {
    const auto& entry = log.entries[index];
    if (index != 0U) {
      out.push_back(',');
    }
    out.append("\n    {\n      \"session_id\": ");
    out.append(std::to_string(entry.session_id));
    out.append(",\n      \"seq_num\": ");
    out.append(std::to_string(entry.seq_num));
    out.append(",\n      \"timestamp_ns\": ");
    out.append(std::to_string(entry.timestamp_ns));
    out.append(",\n      \"direction\": ");
    AppendJsonString(out, DirectionToString(entry.direction));
    out.append(",\n      \"flags\": ");
    out.append(std::to_string(entry.flags));
    out.append(",\n      \"payload\": ");
    AppendHexPayload(out, entry.payload);
    out.append("\n    }");
  }
  if (!log.entries.empty()) {
    out.push_back('\n');
    out.append("  ");
  }
  out.append("]\n}");
  return out;
}

auto
MessageLogFromJson(std::string_view json) -> base::Result<MessageLog>
{
  MessageLogJsonParser parser(json);
  return parser.Parse();
}

auto
WriteMessageLog(const MessageLog& log, const std::filesystem::path& path) -> base::Status
{
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error) {
      return base::Status::IoError("unable to create message log directory '" + parent.string() +
                                   "': " + create_error.message());
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return base::Status::IoError("unable to open message log file '" + path.string() + "'");
  }
  const auto json = MessageLogToJson(log);
  out.write(json.data(), static_cast<std::streamsize>(json.size()));
  if (!out) {
    return base::Status::IoError("unable to write message log file '" + path.string() + "'");
  }
  return base::Status::Ok();
}

auto
ReadMessageLog(const std::filesystem::path& path) -> base::Result<MessageLog>
{
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return base::Status::IoError("unable to open message log file '" + path.string() + "'");
  }

  std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.eof() && in.fail()) {
    return base::Status::IoError("unable to read message log file '" + path.string() + "'");
  }
  return MessageLogFromJson(contents);
}

auto
ReplayController::LoadLog(MessageLog log) -> void
{
  log_ = std::move(log);
  position_ = 0U;
}

auto
ReplayController::SetSpeed(ReplaySpeed speed) -> void
{
  speed_ = speed;
}

auto
ReplayController::SetCallback(ReplayCallback callback) -> void
{
  callback_ = std::move(callback);
}

auto
ReplayController::Run() -> base::Status
{
  if (speed_ == ReplaySpeed::kStep) {
    return base::Status::InvalidArgument("replay Run() is not valid in step mode");
  }

  while (!finished()) {
    if (speed_ == ReplaySpeed::kRealTime && position_ > 0U) {
      const auto previous = log_.entries[position_ - 1U].timestamp_ns;
      const auto current = log_.entries[position_].timestamp_ns;
      if (current > previous) {
        const auto delta = current - previous;
        const auto capped_delta = std::min(delta, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        std::this_thread::sleep_for(std::chrono::nanoseconds(static_cast<std::int64_t>(capped_delta)));
      }
    }

    auto stepped = Step();
    if (!stepped.ok()) {
      return stepped.status();
    }
  }
  return base::Status::Ok();
}

auto
ReplayController::Step() -> base::Result<const MessageLogEntry*>
{
  if (finished()) {
    return static_cast<const MessageLogEntry*>(nullptr);
  }

  const auto* entry = &log_.entries[position_];
  if (callback_) {
    auto status = callback_(*entry);
    if (!status.ok()) {
      return status;
    }
  }
  ++position_;
  return entry;
}

auto
ReplayController::Reset() -> void
{
  position_ = 0U;
}

auto
ReplayController::position() const -> std::size_t
{
  return position_;
}

auto
ReplayController::total_entries() const -> std::size_t
{
  return log_.entries.size();
}

auto
ReplayController::finished() const -> bool
{
  return position_ >= log_.entries.size();
}

} // namespace nimble::runtime
