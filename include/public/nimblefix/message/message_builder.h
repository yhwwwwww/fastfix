#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/message/message_view.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::codec {

struct EncodeOptions;
struct EncodeBuffer;
class PrecompiledTemplateTable;

} // namespace nimble::codec

namespace nimble::message {

class GroupEntryBuilder
{
public:
  GroupEntryBuilder() = default;

  auto set_string(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder&;
  auto set_char(std::uint32_t tag, char value) -> GroupEntryBuilder&;
  auto set_float(std::uint32_t tag, double value) -> GroupEntryBuilder&;
  auto set_boolean(std::uint32_t tag, bool value) -> GroupEntryBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> GroupEntryBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> GroupEntryBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> GroupEntryBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> GroupEntryBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> GroupEntryBuilder& { return set_boolean(tag, value); }
  auto reserve_fields(std::size_t count) -> GroupEntryBuilder&;
  auto reserve_groups(std::size_t count) -> GroupEntryBuilder&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> GroupEntryBuilder&;
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder;

private:
  friend class MessageBuilder;

  struct PathSegment
  {
    std::uint32_t count_tag{ 0 };
    std::size_t entry_index{ 0U };
  };

  explicit GroupEntryBuilder(MessageData* root)
    : root_(root)
  {
  }

  GroupEntryBuilder(MessageData* root, std::vector<PathSegment> path)
    : root_(root)
    , path_(std::move(path))
  {
  }

  auto resolve() -> MessageData*;
  auto upsert_field(FieldValue value) -> GroupEntryBuilder&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData*;

  MessageData* root_{ nullptr };
  std::vector<PathSegment> path_;
};

class MessageBuilder
{
public:
  explicit MessageBuilder(std::string msg_type);

  [[nodiscard]] auto view() const -> MessageView;
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer) const -> base::Status;
  auto encode_to_buffer(const profile::NormalizedDictionaryView& dictionary,
                        const codec::EncodeOptions& options,
                        codec::EncodeBuffer* buffer,
                        const codec::PrecompiledTemplateTable* precompiled) const -> base::Status;
  auto encode(const profile::NormalizedDictionaryView& dictionary, const codec::EncodeOptions& options) const
    -> base::Result<std::vector<std::byte>>;

  auto set_string(std::uint32_t tag, std::string_view value) -> MessageBuilder&;
  auto set_int(std::uint32_t tag, std::int64_t value) -> MessageBuilder&;
  auto set_char(std::uint32_t tag, char value) -> MessageBuilder&;
  auto set_float(std::uint32_t tag, double value) -> MessageBuilder&;
  auto set_boolean(std::uint32_t tag, bool value) -> MessageBuilder&;

  auto set(std::uint32_t tag, std::string_view value) -> MessageBuilder& { return set_string(tag, value); }
  auto set(std::uint32_t tag, std::int64_t value) -> MessageBuilder& { return set_int(tag, value); }
  auto set(std::uint32_t tag, char value) -> MessageBuilder& { return set_char(tag, value); }
  auto set(std::uint32_t tag, double value) -> MessageBuilder& { return set_float(tag, value); }
  auto set(std::uint32_t tag, bool value) -> MessageBuilder& { return set_boolean(tag, value); }
  auto reserve_fields(std::size_t count) -> MessageBuilder&;
  auto reserve_groups(std::size_t count) -> MessageBuilder&;
  auto reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> MessageBuilder&;
  auto add_group_entry(std::uint32_t count_tag) -> GroupEntryBuilder;

  auto build() && -> Message;

  /// Clear fields and groups but preserve allocated capacity for reuse.
  auto reset() -> void;

private:
  auto upsert_field(FieldValue value) -> MessageBuilder&;
  auto ensure_group(std::uint32_t count_tag) -> GroupData&;

  MessageData data_;
};

} // namespace nimble::message