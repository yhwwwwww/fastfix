#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fastfix::session {

inline constexpr std::size_t kEncodedFrameInlineCapacity = 768U;

struct EncodedFrameBytes {
    std::array<std::byte, kEncodedFrameInlineCapacity> inline_storage{};
    std::size_t inline_size{0U};
    std::vector<std::byte> overflow_storage;

    auto assign(std::span<const std::byte> bytes) -> void {
        if (bytes.size() <= kEncodedFrameInlineCapacity) {
            inline_size = bytes.size();
            overflow_storage.clear();
            std::copy(bytes.begin(), bytes.end(), inline_storage.begin());
            return;
        }

        inline_size = 0U;
        overflow_storage.assign(bytes.begin(), bytes.end());
    }

    [[nodiscard]] auto view() const -> std::span<const std::byte> {
        if (!overflow_storage.empty()) {
            return std::span<const std::byte>(overflow_storage.data(), overflow_storage.size());
        }
        return std::span<const std::byte>(inline_storage.data(), inline_size);
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return overflow_storage.empty() ? inline_size : overflow_storage.size();
    }

    [[nodiscard]] auto empty() const -> bool {
        return size() == 0U;
    }

    operator std::span<const std::byte>() const {
        return view();
    }
};

struct EncodedFrame {
    EncodedFrameBytes bytes;
    std::string msg_type;
    bool admin{false};
};

}  // namespace fastfix::session
