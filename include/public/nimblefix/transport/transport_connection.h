#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/transport/tcp_transport.h"

namespace nimble::transport {

namespace detail {

struct TlsConnectionImpl;

} // namespace detail

enum class TransportConnectionKind : std::uint32_t
{
  kPlainTcp = 0,
  kTlsClient,
  kTlsServer,
};

struct TlsSessionInfo
{
  std::string protocol;
  std::string cipher;
  std::string peer_subject;
  bool session_reused{ false };
};

class TlsConnection
{
public:
  TlsConnection();
  ~TlsConnection();

  TlsConnection(const TlsConnection&) = delete;
  auto operator=(const TlsConnection&) -> TlsConnection& = delete;

  TlsConnection(TlsConnection&& other) noexcept;
  auto operator=(TlsConnection&& other) noexcept -> TlsConnection&;

  [[nodiscard]] static auto Connect(const std::string& host,
                                    std::uint16_t port,
                                    std::chrono::milliseconds timeout,
                                    const runtime::TlsClientConfig& config) -> base::Result<TlsConnection>;

  [[nodiscard]] static auto Accept(TcpConnection connection,
                                   std::chrono::milliseconds timeout,
                                   const runtime::TlsServerConfig& config) -> base::Result<TlsConnection>;

  [[nodiscard]] auto valid() const -> bool;
  [[nodiscard]] auto fd() const -> int;
  [[nodiscard]] auto kind() const -> TransportConnectionKind;
  [[nodiscard]] auto session_info() const -> const TlsSessionInfo&;

  auto Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto SendZeroCopyGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>;
  auto ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;
  auto Close() -> void;

private:
  explicit TlsConnection(std::unique_ptr<detail::TlsConnectionImpl> impl);

  std::unique_ptr<detail::TlsConnectionImpl> impl_;
};

class TransportConnection
{
public:
  TransportConnection() = default;

  explicit TransportConnection(TcpConnection connection);
  explicit TransportConnection(TlsConnection connection);

  TransportConnection(const TransportConnection&) = delete;
  auto operator=(const TransportConnection&) -> TransportConnection& = delete;

  TransportConnection(TransportConnection&&) noexcept = default;
  auto operator=(TransportConnection&&) noexcept -> TransportConnection& = default;

  [[nodiscard]] static auto Connect(const std::string& host,
                                    std::uint16_t port,
                                    std::chrono::milliseconds timeout,
                                    const runtime::TlsClientConfig* tls_config = nullptr)
    -> base::Result<TransportConnection>;

  [[nodiscard]] static auto FromAcceptedTcp(TcpConnection connection,
                                            std::chrono::milliseconds timeout,
                                            const runtime::TlsServerConfig* tls_config = nullptr)
    -> base::Result<TransportConnection>;

  [[nodiscard]] auto valid() const -> bool;
  [[nodiscard]] auto fd() const -> int;
  [[nodiscard]] auto kind() const -> TransportConnectionKind;
  [[nodiscard]] auto uses_tls() const -> bool;
  [[nodiscard]] auto tls_session_info() const -> std::optional<TlsSessionInfo>;

  auto Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status;
  auto SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto SendZeroCopyGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
    -> base::Status;
  auto TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>;
  auto ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>;
  auto Close() -> void;

private:
  std::variant<std::monostate, TcpConnection, TlsConnection> storage_;
};

} // namespace nimble::transport
