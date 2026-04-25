#include "nimblefix/transport/transport_connection.h"

#include <type_traits>
#include <utility>

#if defined(NIMBLEFIX_ENABLE_TLS)
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/codec/simd_scan.h"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#endif

namespace nimble::transport {

#if defined(NIMBLEFIX_ENABLE_TLS)

namespace {

constexpr std::size_t kDefaultTlsReadBufferCapacity = 4096U;
constexpr std::size_t kDefaultTlsFrameBufferCapacity = 1024U;
constexpr std::size_t kDefaultTlsReadChunkSize = 4096U;
constexpr std::size_t kMinimumFrameProbeBytes = 12U;
constexpr std::size_t kBodyLengthFieldPrefixSize = 3U;
constexpr std::size_t kChecksumFieldWireSize = 7U;

enum class IoWaitAction : std::uint32_t
{
  kRead = 0,
  kWrite,
};

enum class ReadProgress : std::uint32_t
{
  kWouldBlock = 0,
  kMadeProgress,
};

struct SslContextDeleter
{
  auto operator()(SSL_CTX* ctx) const noexcept -> void
  {
    if (ctx != nullptr) {
      SSL_CTX_free(ctx);
    }
  }
};

struct SslHandleDeleter
{
  auto operator()(SSL* ssl) const noexcept -> void
  {
    if (ssl != nullptr) {
      SSL_free(ssl);
    }
  }
};

using SslContextPtr = std::unique_ptr<SSL_CTX, SslContextDeleter>;
using SslHandlePtr = std::unique_ptr<SSL, SslHandleDeleter>;

auto
OpenSslErrorText(std::string_view prefix) -> std::string
{
  std::string message(prefix);
  unsigned long error_code = 0;
  bool first = true;
  while ((error_code = ERR_get_error()) != 0U) {
    std::array<char, 256> buffer{};
    ERR_error_string_n(error_code, buffer.data(), buffer.size());
    message.append(first ? ": " : " | ");
    message.append(buffer.data());
    first = false;
  }
  return message;
}

auto
TransportStatus(std::string_view prefix) -> base::Status
{
  return base::Status::IoError(OpenSslErrorText(prefix));
}

auto
TlsVersionToNative(runtime::TlsProtocolVersion version) -> int
{
  switch (version) {
    case runtime::TlsProtocolVersion::kSystemDefault:
      return 0;
    case runtime::TlsProtocolVersion::kTls10:
      return TLS1_VERSION;
    case runtime::TlsProtocolVersion::kTls11:
      return TLS1_1_VERSION;
    case runtime::TlsProtocolVersion::kTls12:
      return TLS1_2_VERSION;
    case runtime::TlsProtocolVersion::kTls13:
      return TLS1_3_VERSION;
  }
  return 0;
}

auto
IsIpAddress(std::string_view text) -> bool
{
  in_addr addr4{};
  in6_addr addr6{};
  const std::string value(text);
  return inet_pton(AF_INET, value.c_str(), &addr4) == 1 || inet_pton(AF_INET6, value.c_str(), &addr6) == 1;
}

auto
ApplyProtocolBounds(SSL_CTX* ctx, runtime::TlsProtocolVersion min_version, runtime::TlsProtocolVersion max_version)
  -> base::Status
{
  const auto min_native = TlsVersionToNative(min_version);
  if (min_native != 0 && SSL_CTX_set_min_proto_version(ctx, min_native) != 1) {
    return TransportStatus("SSL_CTX_set_min_proto_version failed");
  }
  const auto max_native = TlsVersionToNative(max_version);
  if (max_native != 0 && SSL_CTX_set_max_proto_version(ctx, max_native) != 1) {
    return TransportStatus("SSL_CTX_set_max_proto_version failed");
  }
  return base::Status::Ok();
}

auto
LoadTrustRoots(SSL_CTX* ctx, const std::filesystem::path& ca_file, const std::filesystem::path& ca_path) -> base::Status
{
  if (!ca_file.empty() || !ca_path.empty()) {
    if (SSL_CTX_load_verify_locations(
          ctx, ca_file.empty() ? nullptr : ca_file.c_str(), ca_path.empty() ? nullptr : ca_path.c_str()) != 1) {
      return TransportStatus("SSL_CTX_load_verify_locations failed");
    }
    return base::Status::Ok();
  }
  if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
    return TransportStatus("SSL_CTX_set_default_verify_paths failed");
  }
  return base::Status::Ok();
}

auto
LoadCertificatePair(SSL_CTX* ctx,
                    const std::filesystem::path& certificate_chain_file,
                    const std::filesystem::path& private_key_file) -> base::Status
{
  if (certificate_chain_file.empty() && private_key_file.empty()) {
    return base::Status::Ok();
  }
  if (SSL_CTX_use_certificate_chain_file(ctx, certificate_chain_file.c_str()) != 1) {
    return TransportStatus("SSL_CTX_use_certificate_chain_file failed");
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, private_key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
    return TransportStatus("SSL_CTX_use_PrivateKey_file failed");
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    return TransportStatus("SSL_CTX_check_private_key failed");
  }
  return base::Status::Ok();
}

auto
ConfigureClientContext(const runtime::TlsClientConfig& config) -> base::Result<SslContextPtr>
{
  OPENSSL_init_ssl(0, nullptr);
  auto ctx = SslContextPtr(SSL_CTX_new(TLS_client_method()));
  if (ctx == nullptr) {
    return TransportStatus("SSL_CTX_new(TLS_client_method) failed");
  }

  auto status = ApplyProtocolBounds(ctx.get(), config.min_version, config.max_version);
  if (!status.ok()) {
    return status;
  }
  if (!config.cipher_list.empty() && SSL_CTX_set_cipher_list(ctx.get(), config.cipher_list.c_str()) != 1) {
    return TransportStatus("SSL_CTX_set_cipher_list failed");
  }
  if (!config.cipher_suites.empty() && SSL_CTX_set_ciphersuites(ctx.get(), config.cipher_suites.c_str()) != 1) {
    return TransportStatus("SSL_CTX_set_ciphersuites failed");
  }
  status = LoadTrustRoots(ctx.get(), config.ca_file, config.ca_path);
  if (!status.ok()) {
    return status;
  }
  status = LoadCertificatePair(ctx.get(), config.certificate_chain_file, config.private_key_file);
  if (!status.ok()) {
    return status;
  }
  SSL_CTX_set_verify(ctx.get(), config.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
  SSL_CTX_set_session_cache_mode(ctx.get(), config.session_resumption ? SSL_SESS_CACHE_CLIENT : SSL_SESS_CACHE_OFF);
  return ctx;
}

auto
ConfigureServerContext(const runtime::TlsServerConfig& config) -> base::Result<SslContextPtr>
{
  OPENSSL_init_ssl(0, nullptr);
  auto ctx = SslContextPtr(SSL_CTX_new(TLS_server_method()));
  if (ctx == nullptr) {
    return TransportStatus("SSL_CTX_new(TLS_server_method) failed");
  }

  auto status = ApplyProtocolBounds(ctx.get(), config.min_version, config.max_version);
  if (!status.ok()) {
    return status;
  }
  if (!config.cipher_list.empty() && SSL_CTX_set_cipher_list(ctx.get(), config.cipher_list.c_str()) != 1) {
    return TransportStatus("SSL_CTX_set_cipher_list failed");
  }
  if (!config.cipher_suites.empty() && SSL_CTX_set_ciphersuites(ctx.get(), config.cipher_suites.c_str()) != 1) {
    return TransportStatus("SSL_CTX_set_ciphersuites failed");
  }
  status = LoadCertificatePair(ctx.get(), config.certificate_chain_file, config.private_key_file);
  if (!status.ok()) {
    return status;
  }
  if (config.verify_peer || config.require_client_certificate) {
    status = LoadTrustRoots(ctx.get(), config.ca_file, config.ca_path);
    if (!status.ok()) {
      return status;
    }
  }

  int verify_mode = SSL_VERIFY_NONE;
  if (config.verify_peer) {
    verify_mode = SSL_VERIFY_PEER;
  }
  if (config.require_client_certificate) {
    verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  SSL_CTX_set_verify(ctx.get(), verify_mode, nullptr);
  SSL_CTX_set_session_cache_mode(ctx.get(), config.session_cache ? SSL_SESS_CACHE_SERVER : SSL_SESS_CACHE_OFF);
  return ctx;
}

template<typename Sink>
auto
TryExtractFrame(std::span<const std::byte> buffer, Sink&& sink) -> base::Result<std::size_t>
{
  if (buffer.size() < kMinimumFrameProbeBytes) {
    return std::size_t{ 0 };
  }
  if (static_cast<char>(buffer[0]) != codec::tags::kBeginStringPrefix.front() ||
      static_cast<char>(buffer[1]) != codec::tags::kBeginStringPrefix[1]) {
    return base::Status::FormatError("FIX frame must begin with tag 8");
  }

  constexpr auto kDelim = std::byte{ '\x01' };
  const auto* first_ptr = codec::FindByte(buffer.data(), buffer.size(), kDelim);
  const auto first_delimiter = static_cast<std::size_t>(first_ptr - buffer.data());
  if (first_delimiter >= buffer.size()) {
    return std::size_t{ 0 };
  }
  if (first_delimiter + kBodyLengthFieldPrefixSize >= buffer.size()) {
    return std::size_t{ 0 };
  }
  if (static_cast<char>(buffer[first_delimiter + 1U]) != codec::tags::kBodyLengthPrefix.front() ||
      static_cast<char>(buffer[first_delimiter + 2U]) != codec::tags::kBodyLengthPrefix[1]) {
    return base::Status::FormatError("FIX frame must place BodyLength immediately after BeginString");
  }

  const auto* scan_start = buffer.data() + first_delimiter + kBodyLengthFieldPrefixSize;
  const auto scan_len = buffer.size() - first_delimiter - kBodyLengthFieldPrefixSize;
  const auto* second_ptr = codec::FindByte(scan_start, scan_len, kDelim);
  const auto second_delimiter = static_cast<std::size_t>(second_ptr - buffer.data());
  if (second_delimiter >= buffer.size()) {
    return std::size_t{ 0 };
  }

  std::uint32_t body_length = 0;
  const auto* begin = reinterpret_cast<const char*>(buffer.data() + first_delimiter + kBodyLengthFieldPrefixSize);
  const auto* end = reinterpret_cast<const char*>(buffer.data() + second_delimiter);
  const auto [ptr, ec] = std::from_chars(begin, end, body_length);
  if (ec != std::errc() || ptr != end) {
    return base::Status::FormatError("invalid BodyLength field in frame header");
  }

  const std::size_t total_size = (second_delimiter + 1U) + body_length + kChecksumFieldWireSize;
  if (buffer.size() < total_size) {
    return std::size_t{ 0 };
  }

  sink(buffer.subspan(0, total_size));
  return total_size;
}

auto
CompactReadBuffer(std::vector<std::byte>& read_buffer, std::size_t& read_cursor, std::size_t consumed) -> void
{
  read_cursor += consumed;
  if (read_cursor == read_buffer.size()) {
    read_buffer.clear();
    read_cursor = 0U;
    return;
  }
  if (read_cursor > read_buffer.capacity() / 2U) {
    read_buffer.erase(read_buffer.begin(), read_buffer.begin() + static_cast<std::ptrdiff_t>(read_cursor));
    read_cursor = 0U;
  }
}

auto
EpollWaitFd(int epoll_fd, int fd, IoWaitAction action, int timeout_ms) -> int
{
  epoll_event event{};
  event.events = action == IoWaitAction::kRead ? EPOLLIN : EPOLLOUT;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
    if (errno == EEXIST) {
      if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
        return -1;
      }
    } else {
      return -1;
    }
  }

  epoll_event ready{};
  int ret = 0;
  do {
    ret = epoll_wait(epoll_fd, &ready, 1, timeout_ms);
  } while (ret == -1 && errno == EINTR);
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  if (ret == 0) {
    return 0;
  }
  if (ret < 0 || (ready.events & (EPOLLERR | EPOLLHUP)) != 0) {
    return -1;
  }
  return 1;
}

auto
RemainingTimeoutMs(std::chrono::steady_clock::time_point deadline) -> int
{
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return remaining.count() > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max()
                                                             : static_cast<int>(remaining.count());
}

auto
PeerSubject(SSL* ssl) -> std::string
{
  X509* certificate = SSL_get1_peer_certificate(ssl);
  if (certificate == nullptr) {
    return {};
  }

  std::array<char, 512> buffer{};
  X509_NAME_oneline(X509_get_subject_name(certificate), buffer.data(), buffer.size());
  X509_free(certificate);
  return std::string(buffer.data());
}

} // namespace

struct detail::TlsConnectionImpl
{
  TcpConnection transport;
  SslContextPtr context;
  SslHandlePtr ssl;
  TransportConnectionKind kind{ TransportConnectionKind::kTlsClient };
  TlsSessionInfo session_info;
  int epoll_fd{ -1 };
  std::vector<std::byte> read_buffer;
  std::vector<std::byte> frame_buffer;
  std::size_t read_cursor{ 0U };

  TlsConnectionImpl()
  {
    read_buffer.reserve(kDefaultTlsReadBufferCapacity);
    frame_buffer.reserve(kDefaultTlsFrameBufferCapacity);
  }

  ~TlsConnectionImpl()
  {
    if (epoll_fd >= 0) {
      ::close(epoll_fd);
      epoll_fd = -1;
    }
  }
};

TlsConnection::TlsConnection() = default;

TlsConnection::TlsConnection(std::unique_ptr<detail::TlsConnectionImpl> impl)
  : impl_(std::move(impl))
{
}

TlsConnection::~TlsConnection()
{
  Close();
}

TlsConnection::TlsConnection(TlsConnection&& other) noexcept = default;

auto
TlsConnection::operator=(TlsConnection&& other) noexcept -> TlsConnection& = default;

namespace {

auto
PrepareSslHandle(detail::TlsConnectionImpl& impl, SSL_CTX* ctx, int fd) -> base::Status
{
  impl.ssl.reset(SSL_new(ctx));
  if (!impl.ssl) {
    return TransportStatus("SSL_new failed");
  }
  SSL_set_mode(impl.ssl.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (SSL_set_fd(impl.ssl.get(), fd) != 1) {
    return TransportStatus("SSL_set_fd failed");
  }
  return base::Status::Ok();
}

auto
WaitForSslIo(detail::TlsConnectionImpl& impl,
             IoWaitAction action,
             std::chrono::steady_clock::time_point deadline,
             std::string_view timeout_message) -> base::Status
{
  if (impl.epoll_fd < 0) {
    impl.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (impl.epoll_fd < 0) {
      return base::Status::IoError("epoll_create1 failed");
    }
  }
  const auto timeout_ms = RemainingTimeoutMs(deadline);
  if (timeout_ms == 0) {
    return base::Status::IoError(std::string(timeout_message));
  }
  const auto result = EpollWaitFd(impl.epoll_fd, impl.transport.fd(), action, timeout_ms);
  if (result == 0) {
    return base::Status::IoError(std::string(timeout_message));
  }
  if (result < 0) {
    return base::Status::IoError("socket closed or errored while polling TLS transport");
  }
  return base::Status::Ok();
}

auto
ApplyPeerVerification(SSL* ssl, std::string_view host, const runtime::TlsClientConfig& config) -> base::Status
{
  if (!config.verify_peer) {
    return base::Status::Ok();
  }

  const std::string peer_name = !config.expected_peer_name.empty() ? config.expected_peer_name
                                : !config.server_name.empty()      ? config.server_name
                                                                   : std::string(host);
  if (peer_name.empty()) {
    return base::Status::InvalidArgument("TLS peer verification requires a host, server_name, or expected_peer_name");
  }

  if (IsIpAddress(peer_name)) {
    auto* params = SSL_get0_param(ssl);
    if (X509_VERIFY_PARAM_set1_ip_asc(params, peer_name.c_str()) != 1) {
      return TransportStatus("X509_VERIFY_PARAM_set1_ip_asc failed");
    }
  } else if (SSL_set1_host(ssl, peer_name.c_str()) != 1) {
    return TransportStatus("SSL_set1_host failed");
  }
  return base::Status::Ok();
}

auto
RecordSessionInfo(detail::TlsConnectionImpl& impl) -> void
{
  impl.session_info.protocol = SSL_get_version(impl.ssl.get());
  impl.session_info.cipher = SSL_get_cipher_name(impl.ssl.get());
  impl.session_info.peer_subject = PeerSubject(impl.ssl.get());
  impl.session_info.session_reused = SSL_session_reused(impl.ssl.get()) == 1;
}

auto
EnsureHandshakeVerified(SSL* ssl, bool verify_peer) -> base::Status
{
  if (!verify_peer) {
    return base::Status::Ok();
  }
  if (SSL_get_verify_result(ssl) != X509_V_OK) {
    return base::Status::IoError("TLS peer verification failed");
  }
  return base::Status::Ok();
}

template<typename HandshakeStep>
auto
PerformHandshake(detail::TlsConnectionImpl& impl,
                 std::chrono::milliseconds timeout,
                 bool verify_peer,
                 HandshakeStep&& handshake_step) -> base::Status
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    ERR_clear_error();
    const auto rc = handshake_step();
    if (rc == 1) {
      auto status = EnsureHandshakeVerified(impl.ssl.get(), verify_peer);
      if (!status.ok()) {
        return status;
      }
      RecordSessionInfo(impl);
      return base::Status::Ok();
    }

    const auto error = SSL_get_error(impl.ssl.get(), rc);
    switch (error) {
      case SSL_ERROR_WANT_READ: {
        auto status = WaitForSslIo(impl, IoWaitAction::kRead, deadline, "timed out during TLS handshake");
        if (!status.ok()) {
          return status;
        }
        break;
      }
      case SSL_ERROR_WANT_WRITE: {
        auto status = WaitForSslIo(impl, IoWaitAction::kWrite, deadline, "timed out during TLS handshake");
        if (!status.ok()) {
          return status;
        }
        break;
      }
      case SSL_ERROR_ZERO_RETURN:
        return base::Status::IoError("peer closed the TLS connection during handshake");
      default:
        return TransportStatus("TLS handshake failed");
    }
  }
}

auto
ReadDecryptedBytes(detail::TlsConnectionImpl& impl, std::optional<std::chrono::steady_clock::time_point> deadline)
  -> base::Result<ReadProgress>
{
  std::array<std::byte, kDefaultTlsReadChunkSize> buffer{};
  while (true) {
    ERR_clear_error();
    const auto rc = SSL_read(impl.ssl.get(), buffer.data(), static_cast<int>(buffer.size()));
    if (rc > 0) {
      impl.read_buffer.insert(impl.read_buffer.end(), buffer.begin(), buffer.begin() + rc);
      if (impl.read_buffer.size() > kMaxReadBufferSize) {
        return base::Status::IoError("TLS read buffer exceeded maximum size limit");
      }
      return ReadProgress::kMadeProgress;
    }

    const auto error = SSL_get_error(impl.ssl.get(), rc);
    switch (error) {
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_WANT_WRITE: {
        if (SSL_pending(impl.ssl.get()) > 0) {
          continue;
        }
        if (!deadline.has_value()) {
          return ReadProgress::kWouldBlock;
        }
        auto status = WaitForSslIo(impl,
                                   error == SSL_ERROR_WANT_READ ? IoWaitAction::kRead : IoWaitAction::kWrite,
                                   *deadline,
                                   "timed out while waiting for a TLS frame");
        if (!status.ok()) {
          return status;
        }
        continue;
      }
      case SSL_ERROR_ZERO_RETURN:
        return base::Status::IoError("peer closed the TLS connection");
      case SSL_ERROR_SYSCALL:
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (!deadline.has_value()) {
            return ReadProgress::kWouldBlock;
          }
          auto status = WaitForSslIo(impl, IoWaitAction::kRead, *deadline, "timed out while waiting for a TLS frame");
          if (!status.ok()) {
            return status;
          }
          continue;
        }
        if (rc == 0) {
          return base::Status::IoError("peer closed the TLS connection");
        }
        return base::Status::IoError(std::string("recv failed: ") + std::strerror(errno));
      default:
        return TransportStatus("SSL_read failed");
    }
  }
}

auto
SendContiguous(detail::TlsConnectionImpl& impl, std::span<const std::byte> bytes, std::chrono::milliseconds timeout)
  -> base::Status
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t sent = 0U;
  while (sent < bytes.size()) {
    ERR_clear_error();
    const auto remaining = bytes.size() - sent;
    const auto write_len = remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())
                             ? std::numeric_limits<int>::max()
                             : static_cast<int>(remaining);
    const auto rc = SSL_write(impl.ssl.get(), bytes.data() + sent, write_len);
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }

    const auto error = SSL_get_error(impl.ssl.get(), rc);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
      auto status = WaitForSslIo(impl,
                                 error == SSL_ERROR_WANT_READ ? IoWaitAction::kRead : IoWaitAction::kWrite,
                                 deadline,
                                 "timed out while sending on TLS transport");
      if (!status.ok()) {
        return status;
      }
      continue;
    }
    if (error == SSL_ERROR_ZERO_RETURN) {
      return base::Status::IoError("peer closed the TLS connection during send");
    }
    return TransportStatus("SSL_write failed");
  }
  return base::Status::Ok();
}

auto
TryReceiveFrameInternal(detail::TlsConnectionImpl& impl, std::optional<std::chrono::steady_clock::time_point> deadline)
  -> base::Result<std::optional<std::span<const std::byte>>>
{
  while (true) {
    const auto unconsumed = std::span<const std::byte>(impl.read_buffer.data() + impl.read_cursor,
                                                       impl.read_buffer.size() - impl.read_cursor);
    auto consumed = TryExtractFrame(
      unconsumed, [&](std::span<const std::byte> frame) { impl.frame_buffer.assign(frame.begin(), frame.end()); });
    if (consumed.ok() && consumed.value() > 0U) {
      CompactReadBuffer(impl.read_buffer, impl.read_cursor, consumed.value());
      return std::optional<std::span<const std::byte>>(
        std::span<const std::byte>(impl.frame_buffer.data(), impl.frame_buffer.size()));
    }
    if (consumed.status().code() == base::ErrorCode::kFormatError) {
      return consumed.status();
    }

    auto read_status = ReadDecryptedBytes(impl, deadline);
    if (!read_status.ok()) {
      return read_status.status();
    }
    if (read_status.value() == ReadProgress::kWouldBlock) {
      return std::optional<std::span<const std::byte>>{};
    }
  }
}

} // namespace

auto
TlsConnection::Connect(const std::string& host,
                       std::uint16_t port,
                       std::chrono::milliseconds timeout,
                       const runtime::TlsClientConfig& config) -> base::Result<TlsConnection>
{
#if !defined(NIMBLEFIX_ENABLE_TLS)
  (void)host;
  (void)port;
  (void)timeout;
  (void)config;
  return base::Status::InvalidArgument("TLS transport support is not enabled in this build");
#else
  auto tcp_connection = TcpConnection::Connect(host, port, timeout);
  if (!tcp_connection.ok()) {
    return tcp_connection.status();
  }

  auto impl = std::make_unique<detail::TlsConnectionImpl>();
  impl->transport = std::move(tcp_connection).value();
  impl->kind = TransportConnectionKind::kTlsClient;

  auto context = ConfigureClientContext(config);
  if (!context.ok()) {
    return context.status();
  }
  impl->context = std::move(context).value();

  auto status = PrepareSslHandle(*impl, impl->context.get(), impl->transport.fd());
  if (!status.ok()) {
    return status;
  }

  const std::string sni_name = !config.server_name.empty() ? config.server_name : host;
  if (!sni_name.empty() && SSL_set_tlsext_host_name(impl->ssl.get(), sni_name.c_str()) != 1) {
    return TransportStatus("SSL_set_tlsext_host_name failed");
  }
  status = ApplyPeerVerification(impl->ssl.get(), host, config);
  if (!status.ok()) {
    return status;
  }
  status = PerformHandshake(*impl, timeout, config.verify_peer, [&]() { return SSL_connect(impl->ssl.get()); });
  if (!status.ok()) {
    return status;
  }

  return TlsConnection(std::move(impl));
#endif
}

auto
TlsConnection::Accept(TcpConnection connection,
                      std::chrono::milliseconds timeout,
                      const runtime::TlsServerConfig& config) -> base::Result<TlsConnection>
{
#if !defined(NIMBLEFIX_ENABLE_TLS)
  (void)connection;
  (void)timeout;
  (void)config;
  return base::Status::InvalidArgument("TLS transport support is not enabled in this build");
#else
  auto impl = std::make_unique<detail::TlsConnectionImpl>();
  impl->transport = std::move(connection);
  impl->kind = TransportConnectionKind::kTlsServer;

  auto context = ConfigureServerContext(config);
  if (!context.ok()) {
    return context.status();
  }
  impl->context = std::move(context).value();

  auto status = PrepareSslHandle(*impl, impl->context.get(), impl->transport.fd());
  if (!status.ok()) {
    return status;
  }
  status = PerformHandshake(*impl, timeout, config.verify_peer || config.require_client_certificate, [&]() {
    return SSL_accept(impl->ssl.get());
  });
  if (!status.ok()) {
    return status;
  }

  return TlsConnection(std::move(impl));
#endif
}

auto
TlsConnection::valid() const -> bool
{
  return impl_ != nullptr && impl_->transport.valid() && impl_->ssl != nullptr;
}

auto
TlsConnection::fd() const -> int
{
  return impl_ != nullptr ? impl_->transport.fd() : -1;
}

auto
TlsConnection::kind() const -> TransportConnectionKind
{
  return impl_ != nullptr ? impl_->kind : TransportConnectionKind::kPlainTcp;
}

auto
TlsConnection::session_info() const -> const TlsSessionInfo&
{
  static const TlsSessionInfo kEmptyInfo{};
  return impl_ != nullptr ? impl_->session_info : kEmptyInfo;
}

auto
TlsConnection::Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status
{
  if (impl_ == nullptr) {
    return base::Status::IoError("TLS connection is not initialized");
  }
  return SendContiguous(*impl_, bytes, timeout);
}

auto
TlsConnection::Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status
{
  return Send(std::span<const std::byte>(bytes.data(), bytes.size()), timeout);
}

auto
TlsConnection::SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
  -> base::Status
{
  for (const auto& segment : segments) {
    auto status = Send(segment, timeout);
    if (!status.ok()) {
      return status;
    }
  }
  return base::Status::Ok();
}

auto
TlsConnection::SendZeroCopyGather(std::span<const std::span<const std::byte>> segments,
                                  std::chrono::milliseconds timeout) -> base::Status
{
  return SendGather(segments, timeout);
}

auto
TlsConnection::TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>
{
  if (impl_ == nullptr) {
    return base::Status::IoError("TLS connection is not initialized");
  }
  return TryReceiveFrameInternal(*impl_, std::nullopt);
}

auto
TlsConnection::ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>
{
  if (impl_ == nullptr) {
    return base::Status::IoError("TLS connection is not initialized");
  }
  auto frame = TryReceiveFrameInternal(*impl_, std::chrono::steady_clock::now() + timeout);
  if (!frame.ok()) {
    return frame.status();
  }
  if (!frame.value().has_value()) {
    return base::Status::IoError("timed out while waiting for a TLS frame");
  }
  return frame.value().value();
}

auto
TlsConnection::Close() -> void
{
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->ssl != nullptr) {
    ERR_clear_error();
    SSL_shutdown(impl_->ssl.get());
    impl_->ssl.reset();
  }
  impl_->transport.Close();
  impl_->read_buffer.clear();
  impl_->frame_buffer.clear();
  impl_->read_cursor = 0U;
}

#else

struct detail::TlsConnectionImpl
{};

TlsConnection::TlsConnection() = default;

TlsConnection::TlsConnection(std::unique_ptr<detail::TlsConnectionImpl> impl)
  : impl_(std::move(impl))
{
}

TlsConnection::~TlsConnection() = default;

TlsConnection::TlsConnection(TlsConnection&& other) noexcept = default;

auto
TlsConnection::operator=(TlsConnection&& other) noexcept -> TlsConnection& = default;

auto
TlsConnection::Connect(const std::string& host,
                       std::uint16_t port,
                       std::chrono::milliseconds timeout,
                       const runtime::TlsClientConfig& config) -> base::Result<TlsConnection>
{
  (void)host;
  (void)port;
  (void)timeout;
  (void)config;
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::Accept(TcpConnection connection,
                      std::chrono::milliseconds timeout,
                      const runtime::TlsServerConfig& config) -> base::Result<TlsConnection>
{
  (void)connection;
  (void)timeout;
  (void)config;
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::valid() const -> bool
{
  return false;
}

auto
TlsConnection::fd() const -> int
{
  return -1;
}

auto
TlsConnection::kind() const -> TransportConnectionKind
{
  return TransportConnectionKind::kPlainTcp;
}

auto
TlsConnection::session_info() const -> const TlsSessionInfo&
{
  static const TlsSessionInfo kEmptyInfo{};
  return kEmptyInfo;
}

auto
TlsConnection::Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status
{
  (void)bytes;
  (void)timeout;
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status
{
  return Send(std::span<const std::byte>(bytes.data(), bytes.size()), timeout);
}

auto
TlsConnection::SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
  -> base::Status
{
  (void)segments;
  (void)timeout;
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::SendZeroCopyGather(std::span<const std::span<const std::byte>> segments,
                                  std::chrono::milliseconds timeout) -> base::Status
{
  return SendGather(segments, timeout);
}

auto
TlsConnection::TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>
{
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>
{
  (void)timeout;
  return base::Status::InvalidArgument(
    "runtime TLS was requested but this build was compiled without optional TLS support");
}

auto
TlsConnection::Close() -> void
{
  impl_.reset();
}

#endif

TransportConnection::TransportConnection(TcpConnection connection)
  : storage_(std::move(connection))
{
}

TransportConnection::TransportConnection(TlsConnection connection)
  : storage_(std::move(connection))
{
}

auto
TransportConnection::Connect(const std::string& host,
                             std::uint16_t port,
                             std::chrono::milliseconds timeout,
                             const runtime::TlsClientConfig* tls_config) -> base::Result<TransportConnection>
{
  if (tls_config != nullptr && tls_config->enabled) {
    auto tls_connection = TlsConnection::Connect(host, port, timeout, *tls_config);
    if (!tls_connection.ok()) {
      return tls_connection.status();
    }
    return TransportConnection(std::move(tls_connection).value());
  }

  auto tcp_connection = TcpConnection::Connect(host, port, timeout);
  if (!tcp_connection.ok()) {
    return tcp_connection.status();
  }
  return TransportConnection(std::move(tcp_connection).value());
}

auto
TransportConnection::FromAcceptedTcp(TcpConnection connection,
                                     std::chrono::milliseconds timeout,
                                     const runtime::TlsServerConfig* tls_config) -> base::Result<TransportConnection>
{
  if (tls_config != nullptr && tls_config->enabled) {
    auto tls_connection = TlsConnection::Accept(std::move(connection), timeout, *tls_config);
    if (!tls_connection.ok()) {
      return tls_connection.status();
    }
    return TransportConnection(std::move(tls_connection).value());
  }

  (void)timeout;
  return TransportConnection(std::move(connection));
}

auto
TransportConnection::valid() const -> bool
{
  return std::visit(
    [](const auto& connection) -> bool {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return false;
      } else {
        return connection.valid();
      }
    },
    storage_);
}

auto
TransportConnection::fd() const -> int
{
  return std::visit(
    [](const auto& connection) -> int {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return -1;
      } else {
        return connection.fd();
      }
    },
    storage_);
}

auto
TransportConnection::kind() const -> TransportConnectionKind
{
  return std::visit(
    [](const auto& connection) -> TransportConnectionKind {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return TransportConnectionKind::kPlainTcp;
      } else if constexpr (std::is_same_v<Connection, TcpConnection>) {
        return TransportConnectionKind::kPlainTcp;
      } else {
        return connection.kind();
      }
    },
    storage_);
}

auto
TransportConnection::uses_tls() const -> bool
{
  return kind() != TransportConnectionKind::kPlainTcp;
}

auto
TransportConnection::tls_session_info() const -> std::optional<TlsSessionInfo>
{
  if (const auto* tls = std::get_if<TlsConnection>(&storage_)) {
    return tls->session_info();
  }
  return std::nullopt;
}

auto
TransportConnection::Send(std::span<const std::byte> bytes, std::chrono::milliseconds timeout) -> base::Status
{
  return std::visit(
    [&](auto& connection) -> base::Status {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return base::Status::IoError("transport connection is not initialized");
      } else {
        return connection.Send(bytes, timeout);
      }
    },
    storage_);
}

auto
TransportConnection::Send(const std::vector<std::byte>& bytes, std::chrono::milliseconds timeout) -> base::Status
{
  return Send(std::span<const std::byte>(bytes.data(), bytes.size()), timeout);
}

auto
TransportConnection::SendGather(std::span<const std::span<const std::byte>> segments, std::chrono::milliseconds timeout)
  -> base::Status
{
  return std::visit(
    [&](auto& connection) -> base::Status {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return base::Status::IoError("transport connection is not initialized");
      } else {
        return connection.SendGather(segments, timeout);
      }
    },
    storage_);
}

auto
TransportConnection::SendZeroCopyGather(std::span<const std::span<const std::byte>> segments,
                                        std::chrono::milliseconds timeout) -> base::Status
{
  return std::visit(
    [&](auto& connection) -> base::Status {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return base::Status::IoError("transport connection is not initialized");
      } else {
        return connection.SendZeroCopyGather(segments, timeout);
      }
    },
    storage_);
}

auto
TransportConnection::TryReceiveFrameView() -> base::Result<std::optional<std::span<const std::byte>>>
{
  return std::visit(
    [](auto& connection) -> base::Result<std::optional<std::span<const std::byte>>> {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return base::Status::IoError("transport connection is not initialized");
      } else {
        return connection.TryReceiveFrameView();
      }
    },
    storage_);
}

auto
TransportConnection::ReceiveFrameView(std::chrono::milliseconds timeout) -> base::Result<std::span<const std::byte>>
{
  return std::visit(
    [&](auto& connection) -> base::Result<std::span<const std::byte>> {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (std::is_same_v<Connection, std::monostate>) {
        return base::Status::IoError("transport connection is not initialized");
      } else {
        return connection.ReceiveFrameView(timeout);
      }
    },
    storage_);
}

auto
TransportConnection::Close() -> void
{
  std::visit(
    [](auto& connection) -> void {
      using Connection = std::decay_t<decltype(connection)>;
      if constexpr (!std::is_same_v<Connection, std::monostate>) {
        connection.Close();
      }
    },
    storage_);
}

} // namespace nimble::transport
