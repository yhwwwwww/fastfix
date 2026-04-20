#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "nimblefix/message/message_view.h"

namespace nimble::message {

class MessageRef
{
public:
  MessageRef() = default;
  ~MessageRef();
  MessageRef(const MessageRef& other);
  MessageRef(MessageRef&& other) noexcept;

  static auto Take(Message&& message) -> MessageRef;
  static auto Borrow(MessageView view) -> MessageRef;
  static auto Copy(MessageView view) -> MessageRef;
  static auto CopyParsed(ParsedMessage parsed, std::span<const std::byte> raw) -> MessageRef;
  static auto TakeParsed(ParsedMessage parsed, std::vector<std::byte>&& raw) -> MessageRef;

  auto operator=(const MessageRef& other) -> MessageRef&;
  auto operator=(MessageRef&& other) noexcept -> MessageRef&;

  [[nodiscard]] bool valid() const
  {
    if (owned_ != nullptr) {
      return owned_->valid();
    }
    if (parsed_owned_ != nullptr) {
      return parsed_owned_->parsed.valid();
    }
    return view_.valid();
  }

  [[nodiscard]] bool owns_storage() const { return owned_ != nullptr || parsed_owned_ != nullptr; }

  [[nodiscard]] bool borrows_view() const { return owned_ == nullptr && parsed_owned_ == nullptr && view_.valid(); }

  [[nodiscard]] auto view() const -> MessageView
  {
    if (owned_ != nullptr) {
      return owned_->view();
    }
    if (parsed_owned_ != nullptr) {
      return parsed_owned_->parsed.view();
    }
    return view_;
  }

  [[nodiscard]] auto CopyToOwned() const -> Message;

private:
  explicit MessageRef(std::shared_ptr<const Message> owned)
    : owned_(std::move(owned))
  {
  }

  struct ParsedStorage
  {
    std::vector<std::byte> raw;
    ParsedMessage parsed;
  };

  explicit MessageRef(std::shared_ptr<const ParsedStorage> parsed_owned)
    : parsed_owned_(std::move(parsed_owned))
  {
  }

  explicit MessageRef(MessageView borrowed_view)
    : view_(borrowed_view)
  {
  }

  std::shared_ptr<const Message> owned_{};
  std::shared_ptr<const ParsedStorage> parsed_owned_{};
  MessageView view_{};
};

} // namespace nimble::message