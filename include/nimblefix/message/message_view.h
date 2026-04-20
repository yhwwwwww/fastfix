#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "nimblefix/message/message_structs.h"

namespace nimble::message {

class Message;
class MessageRef;
class MessageView;
class RawGroupView;

class ParsedMessage
{
public:
  ParsedMessage() = default;

  explicit ParsedMessage(ParsedMessageData data)
    : data_(std::move(data))
  {
  }

  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] const ParsedMessageData& data() const { return data_; }
  auto RebindRaw(std::span<const std::byte> raw) -> void;
  [[nodiscard]] auto valid() const -> bool
  {
    return !data_.msg_type.empty() || data_.root.field_count != 0U || data_.root.group_count != 0U;
  }
  [[nodiscard]] auto mutable_data() -> ParsedMessageData& { return data_; }
  [[nodiscard]] auto ToOwned() const -> Message;

private:
  ParsedMessageData data_;
};

class Message
{
public:
  Message() = default;

  explicit Message(MessageData data)
    : data_(std::move(data))
  {
  }

  [[nodiscard]] auto view() const -> MessageView;
  [[nodiscard]] bool valid() const { return !data_.msg_type.empty() || !data_.fields.empty() || !data_.groups.empty(); }
  [[nodiscard]] const MessageData& data() const { return data_; }

private:
  MessageData data_;
};

struct RawFieldView
{
  std::uint32_t tag{ 0 };
  std::string_view value;
};

class RawGroupEntryView
{
public:
  RawGroupEntryView() = default;

  RawGroupEntryView(const ParsedMessageData* parsed, const ParsedEntryData* parsed_entry)
    : parsed_(parsed)
    , parsed_entry_(parsed_entry)
  {
  }

  [[nodiscard]] auto valid() const -> bool { return parsed_ != nullptr && parsed_entry_ != nullptr; }

  [[nodiscard]] auto field_count() const -> std::size_t;
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<RawFieldView>;
  [[nodiscard]] auto field(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<RawGroupView>;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedEntryData* parsed_entry_{ nullptr };
};

class RawGroupView
{
public:
  class Iterator
  {
  public:
    Iterator() = default;

    Iterator(const ParsedMessageData* parsed, const ParsedGroupFrame* group, std::uint32_t entry_index)
      : parsed_(parsed)
      , parsed_group_(group)
      , parsed_entry_index_(entry_index)
    {
    }

    [[nodiscard]] auto operator*() const -> RawGroupEntryView;

    auto operator++() -> Iterator&
    {
      if (parsed_ != nullptr && parsed_entry_index_ != kInvalidParsedIndex) {
        parsed_entry_index_ = parsed_->entries[parsed_entry_index_].next_entry;
      }
      return *this;
    }

    [[nodiscard]] bool operator==(const Iterator& other) const
    {
      return parsed_ == other.parsed_ && parsed_group_ == other.parsed_group_ &&
             parsed_entry_index_ == other.parsed_entry_index_;
    }

  private:
    const ParsedMessageData* parsed_{ nullptr };
    const ParsedGroupFrame* parsed_group_{ nullptr };
    std::uint32_t parsed_entry_index_{ kInvalidParsedIndex };
  };

  RawGroupView() = default;

  RawGroupView(const ParsedMessageData* parsed, const ParsedGroupFrame* group)
    : parsed_(parsed)
    , parsed_group_(group)
  {
  }

  [[nodiscard]] auto valid() const -> bool { return parsed_ != nullptr && parsed_group_ != nullptr; }

  [[nodiscard]] auto count_tag() const -> std::uint32_t
  {
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  [[nodiscard]] auto size() const -> std::size_t { return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count; }

  [[nodiscard]] auto operator[](std::size_t index) const -> RawGroupEntryView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

class GroupView
{
public:
  class Iterator
  {
  public:
    Iterator() = default;

    Iterator(const std::vector<MessageData>* entries, std::size_t index)
      : entries_(entries)
      , index_(index)
    {
    }

    Iterator(const ParsedMessageData* parsed, const ParsedGroupFrame* group, std::uint32_t entry_index)
      : parsed_(parsed)
      , parsed_group_(group)
      , parsed_entry_index_(entry_index)
    {
    }

    [[nodiscard]] auto operator*() const -> MessageView;

    auto operator++() -> Iterator&
    {
      if (entries_ != nullptr) {
        ++index_;
        return *this;
      }
      if (parsed_ != nullptr && parsed_entry_index_ != kInvalidParsedIndex) {
        parsed_entry_index_ = parsed_->entries[parsed_entry_index_].next_entry;
      }
      return *this;
    }

    [[nodiscard]] bool operator==(const Iterator& other) const
    {
      return entries_ == other.entries_ && index_ == other.index_ && parsed_ == other.parsed_ &&
             parsed_group_ == other.parsed_group_ && parsed_entry_index_ == other.parsed_entry_index_;
    }

  private:
    const std::vector<MessageData>* entries_{ nullptr };
    std::size_t index_{ 0 };
    const ParsedMessageData* parsed_{ nullptr };
    const ParsedGroupFrame* parsed_group_{ nullptr };
    std::uint32_t parsed_entry_index_{ kInvalidParsedIndex };
  };

  GroupView() = default;

  explicit GroupView(const GroupData* group)
    : group_(group)
  {
  }

  GroupView(const ParsedMessageData* parsed, const ParsedGroupFrame* group)
    : parsed_(parsed)
    , parsed_group_(group)
  {
  }

  [[nodiscard]] bool valid() const { return group_ != nullptr || (parsed_ != nullptr && parsed_group_ != nullptr); }

  [[nodiscard]] std::uint32_t count_tag() const
  {
    if (group_ != nullptr) {
      return group_->count_tag;
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->count_tag;
  }

  [[nodiscard]] std::size_t size() const
  {
    if (group_ != nullptr) {
      return group_->entries.size();
    }
    return parsed_group_ == nullptr ? 0U : parsed_group_->entry_count;
  }

  [[nodiscard]] auto operator[](std::size_t index) const -> MessageView;
  [[nodiscard]] auto begin() const -> Iterator;
  [[nodiscard]] auto end() const -> Iterator;

private:
  const GroupData* group_{ nullptr };
  const ParsedMessageData* parsed_{ nullptr };
  const ParsedGroupFrame* parsed_group_{ nullptr };
};

class MessageView
{
public:
  MessageView() = default;

  explicit MessageView(const MessageData* data)
    : data_(data)
  {
  }

  MessageView(const ParsedMessageData* parsed, std::string_view msg_type, const ParsedEntryData* parsed_entry)
    : parsed_(parsed)
    , parsed_msg_type_(msg_type)
    , parsed_entry_(parsed_entry)
  {
  }

  [[nodiscard]] bool valid() const { return data_ != nullptr || (parsed_ != nullptr && parsed_entry_ != nullptr); }

  [[nodiscard]] std::string_view msg_type() const
  {
    if (data_ != nullptr) {
      return std::string_view(data_->msg_type);
    }
    return parsed_msg_type_;
  }

  [[nodiscard]] auto fields() const -> const std::vector<FieldValue>&
  {
    static const std::vector<FieldValue> empty;
    return data_ == nullptr ? empty : data_->fields;
  }

  [[nodiscard]] auto groups() const -> const std::vector<GroupData>&
  {
    static const std::vector<GroupData> empty;
    return data_ == nullptr ? empty : data_->groups;
  }

  [[nodiscard]] auto field_count() const -> std::size_t;
  [[nodiscard]] auto field_at(std::size_t index) const -> std::optional<FieldView>;
  [[nodiscard]] auto group_count() const -> std::size_t;
  [[nodiscard]] auto group_at(std::size_t index) const -> std::optional<GroupView>;
  [[nodiscard]] auto find_field_view(std::uint32_t tag) const -> std::optional<FieldView>;
  [[nodiscard]] bool has_field(std::uint32_t tag) const;
  [[nodiscard]] auto find_field(std::uint32_t tag) const -> const FieldValue*;
  [[nodiscard]] auto get_string(std::uint32_t tag) const -> std::optional<std::string_view>;
  [[nodiscard]] auto get_int(std::uint32_t tag) const -> std::optional<std::int64_t>;
  [[nodiscard]] auto get_char(std::uint32_t tag) const -> std::optional<char>;
  [[nodiscard]] auto get_float(std::uint32_t tag) const -> std::optional<double>;
  [[nodiscard]] auto get_boolean(std::uint32_t tag) const -> std::optional<bool>;
  [[nodiscard]] auto group(std::uint32_t count_tag) const -> std::optional<GroupView>;
  [[nodiscard]] auto raw_group(std::uint32_t count_tag) const -> std::optional<RawGroupView>;
  /// Check the quick cache before falling back to the full hash lookup.
  [[nodiscard]] auto find_quick_cached(std::uint32_t tag) const -> std::optional<FieldView>;

private:
  const MessageData* data_{ nullptr };
  const ParsedMessageData* parsed_{ nullptr };
  std::string_view parsed_msg_type_;
  const ParsedEntryData* parsed_entry_{ nullptr };

  friend class MessageRef;
};

auto
MaterializeMessage(MessageView view) -> Message;

} // namespace nimble::message