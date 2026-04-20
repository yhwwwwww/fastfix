#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "nimblefix/base/inline_split_vector.h"
#include "nimblefix/codec/fix_tags.h"

namespace nimble::message {

/// Type index for field values — matches
/// std::variant<string,int64,double,char,bool> index.
using FieldTypeIndex = std::uint8_t;
inline constexpr FieldTypeIndex kFieldString = 0;
inline constexpr FieldTypeIndex kFieldInt = 1;
inline constexpr FieldTypeIndex kFieldFloat = 2;
inline constexpr FieldTypeIndex kFieldChar = 3;
inline constexpr FieldTypeIndex kFieldBoolean = 4;

struct FieldValue
{
  std::uint32_t tag{ 0 };
  std::variant<std::string, std::int64_t, double, char, bool> value;

  [[nodiscard]] auto type_index() const -> FieldTypeIndex { return static_cast<FieldTypeIndex>(value.index()); }

  [[nodiscard]] auto& as_string() const { return std::get<std::string>(value); }
  [[nodiscard]] auto as_int() const { return std::get<std::int64_t>(value); }
  [[nodiscard]] auto as_float() const { return std::get<double>(value); }
  [[nodiscard]] auto as_char() const { return std::get<char>(value); }
  [[nodiscard]] auto as_bool() const { return std::get<bool>(value); }
};

struct FieldView
{
  std::uint32_t tag{ 0 };
  FieldTypeIndex type{ kFieldString };
  std::string_view string_value;
  std::int64_t int_value{ 0 };
  double float_value{ 0.0 };
  char char_value{ '\0' };
  bool bool_value{ false };

  [[nodiscard]] auto to_owned() const -> FieldValue
  {
    FieldValue v;
    v.tag = tag;
    switch (type) {
      case kFieldInt:
        v.value = int_value;
        break;
      case kFieldFloat:
        v.value = float_value;
        break;
      case kFieldChar:
        v.value = char_value;
        break;
      case kFieldBoolean:
        v.value = bool_value;
        break;
      default:
        v.value = std::string(string_value);
        break;
    }
    return v;
  }
};

inline constexpr std::uint32_t kInvalidParsedIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint16_t kInvalidHashSlot = 0xFFFFU;
inline constexpr std::size_t kFieldHashTableSize = 1024U;
inline constexpr std::size_t kParsedFieldSlotInlineCapacity = 32U;
inline constexpr std::size_t kParsedEntryInlineCapacity = 4U;
inline constexpr std::size_t kParsedGroupInlineCapacity = 4U;

/// Quick cache for session-critical tags. Index by QuickCacheSlot enum.
enum class QuickCacheSlot : std::uint8_t
{
  kBeginString = 0,  // tag 8
  kBodyLength = 1,   // tag 9
  kMsgType = 2,      // tag 35
  kMsgSeqNum = 3,    // tag 34
  kSenderCompID = 4, // tag 49
  kTargetCompID = 5, // tag 56
  kSendingTime = 6,  // tag 52
  kCheckSum = 7,     // tag 10
  kCount = 8
};

inline constexpr std::size_t kQuickCacheSize = static_cast<std::size_t>(QuickCacheSlot::kCount);

/// Returns the quick-cache slot for a session tag, or nullopt if the tag
/// is not one of the 8 cached session tags.
[[nodiscard]] inline auto
QuickCacheSlotForTag(std::uint32_t tag) -> std::optional<QuickCacheSlot>
{
  switch (tag) {
    case codec::tags::kBeginString:
      return QuickCacheSlot::kBeginString;
    case codec::tags::kBodyLength:
      return QuickCacheSlot::kBodyLength;
    case codec::tags::kMsgType:
      return QuickCacheSlot::kMsgType;
    case codec::tags::kMsgSeqNum:
      return QuickCacheSlot::kMsgSeqNum;
    case codec::tags::kSenderCompID:
      return QuickCacheSlot::kSenderCompID;
    case codec::tags::kTargetCompID:
      return QuickCacheSlot::kTargetCompID;
    case codec::tags::kSendingTime:
      return QuickCacheSlot::kSendingTime;
    case codec::tags::kCheckSum:
      return QuickCacheSlot::kCheckSum;
    default:
      return std::nullopt;
  }
}

struct ParsedFieldSlot
{
  std::uint32_t tag{ 0 };
  std::uint32_t value_offset{ 0 };
  std::uint16_t value_length{ 0 };
  std::uint16_t flags{ 0 }; // lower 3 bits: FieldTypeIndex

  [[nodiscard]] auto type() const -> FieldTypeIndex { return static_cast<FieldTypeIndex>(flags & 0x07U); }
  auto set_type(FieldTypeIndex t) -> void
  {
    flags = static_cast<std::uint16_t>((flags & ~0x07U) | (static_cast<std::uint16_t>(t) & 0x07U));
  }
};

struct ParsedEntryData
{
  std::uint32_t first_field_index{ kInvalidParsedIndex };
  std::uint16_t field_count{ 0 };
  std::uint32_t first_group{ kInvalidParsedIndex };
  std::uint32_t last_group{ kInvalidParsedIndex };
  std::uint16_t group_count{ 0 };
  std::uint32_t next_entry{ kInvalidParsedIndex };
};

struct ParsedGroupFrame
{
  std::uint32_t count_tag{ 0 };
  std::uint32_t first_entry{ kInvalidParsedIndex };
  std::uint32_t last_entry{ kInvalidParsedIndex };
  std::uint16_t entry_count{ 0 };
  std::uint16_t depth{ 0 };
  std::uint32_t next_group{ kInvalidParsedIndex };
};

struct ParsedMessageData
{
  std::span<const std::byte> raw;
  std::string_view msg_type;
  ParsedEntryData root;
  base::InlineSplitVector<ParsedFieldSlot, kParsedFieldSlotInlineCapacity> field_slots;
  base::InlineSplitVector<ParsedEntryData, kParsedEntryInlineCapacity> entries;
  base::InlineSplitVector<ParsedGroupFrame, kParsedGroupInlineCapacity> groups;
  /// Direct-address lookup table for root-level fields. For tags 0..1023
  /// the index IS the tag number. Each entry packs a 16-bit generation
  /// counter (high half) and a 16-bit slot index (low half) into a uint32_t.
  /// A lookup is valid only when the stored generation matches
  /// `field_generation`. Tags >= 1024 fall back to `field_hash_overflow`.
  std::array<std::uint32_t, kFieldHashTableSize> field_hash_table{};
  std::vector<std::uint16_t> field_hash_overflow;
  std::uint16_t field_generation{ 1 };
  std::array<std::uint16_t, kQuickCacheSize> quick_cache;

  ParsedMessageData()
  {
    // field_hash_table is zero-initialized; generation 0 in every entry
    // won't match field_generation == 1, so no explicit fill is needed.
    quick_cache.fill(kInvalidHashSlot);
  }

  /// Prepare for reuse without clearing the 4 KB direct-address table.
  /// Just bumps the generation counter (and handles wrap-around).
  auto ResetForNewDecode() -> void
  {
    ++field_generation;
    if (field_generation == 0U) {
      // Wrapped around to 0 which matches zero-initialized entries.
      field_hash_table.fill(0U);
      field_generation = 1U;
    }
    field_hash_overflow.clear();
    quick_cache.fill(kInvalidHashSlot);
    msg_type = {};
    root = {};
    field_slots.clear();
    entries.clear();
    groups.clear();
  }
};

struct MessageData;

struct GroupData
{
  std::uint32_t count_tag{ 0 };
  std::vector<MessageData> entries;
};

struct MessageData
{
  std::string msg_type;
  std::vector<FieldValue> fields;
  std::vector<GroupData> groups;
};

} // namespace nimble::message
