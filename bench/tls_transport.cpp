#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/transport/tcp_transport.h"
#include "nimblefix/transport/transport_connection.h"

namespace {

struct Options
{
  std::uint32_t iterations{ 10000U };
  std::uint32_t warmup{ 1000U };
  std::filesystem::path certificate_chain_file;
  std::filesystem::path private_key_file;
  std::filesystem::path ca_file;
  bool gather_send{ true };
};

struct Stats
{
  std::size_t count{ 0U };
  double avg_ns{ 0.0 };
  std::uint64_t min_ns{ 0U };
  std::uint64_t p50_ns{ 0U };
  std::uint64_t p95_ns{ 0U };
  std::uint64_t p99_ns{ 0U };
  std::uint64_t max_ns{ 0U };
};

struct RunResult
{
  std::string label;
  std::uint64_t connect_ns{ 0U };
  Stats rtt;
  std::optional<nimble::transport::TlsSessionInfo> tls_info;
};

auto
Now() -> std::chrono::steady_clock::time_point
{
  return std::chrono::steady_clock::now();
}

auto
DurationNs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) -> std::uint64_t
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

auto
Compute(std::vector<std::uint64_t> values) -> Stats
{
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  const auto count = values.size();
  const auto sum = std::accumulate(values.begin(), values.end(), 0.0);
  return Stats{
    .count = count,
    .avg_ns = sum / static_cast<double>(count),
    .min_ns = values.front(),
    .p50_ns = values[count * 50U / 100U],
    .p95_ns = values[count * 95U / 100U],
    .p99_ns = values[count * 99U / 100U],
    .max_ns = values.back(),
  };
}

auto
Bytes(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

auto
EncodeFixFrame(std::string_view body_fields) -> std::vector<std::byte>
{
  std::string body(body_fields);
  for (auto& ch : body) {
    if (ch == '|') {
      ch = '\x01';
    }
  }

  std::string full;
  full.append("8=FIX.4.4\x01");
  full.append("9=");
  full.append(std::to_string(body.size()));
  full.push_back('\x01');
  full.append(body);

  std::uint32_t checksum = 0U;
  for (const auto ch : full) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  std::ostringstream stream;
  stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
  full.append(stream.str());
  return Bytes(full);
}

auto
SendFrame(nimble::transport::TransportConnection& connection, std::span<const std::byte> frame, bool gather_send)
  -> nimble::base::Status
{
  if (!gather_send || frame.size() < 2U) {
    return connection.Send(frame, std::chrono::seconds(5));
  }
  const auto split = frame.size() / 2U;
  std::array<std::span<const std::byte>, 2> segments{ frame.subspan(0U, split), frame.subspan(split) };
  return connection.SendGather(segments, std::chrono::seconds(5));
}

auto
MakeServerTls(const Options& options) -> nimble::runtime::TlsServerConfig
{
  return nimble::runtime::TlsServerConfig{
    .enabled = true,
    .certificate_chain_file = options.certificate_chain_file,
    .private_key_file = options.private_key_file,
    .min_version = nimble::runtime::TlsProtocolVersion::kTls12,
    .max_version = nimble::runtime::TlsProtocolVersion::kTls13,
  };
}

auto
MakeClientTls(const Options& options) -> nimble::runtime::TlsClientConfig
{
  return nimble::runtime::TlsClientConfig{
    .enabled = true,
    .server_name = "localhost",
    .expected_peer_name = "localhost",
    .ca_file = options.ca_file,
    .min_version = nimble::runtime::TlsProtocolVersion::kTls12,
    .max_version = nimble::runtime::TlsProtocolVersion::kTls13,
  };
}

auto
RunEcho(bool use_tls, const Options& options) -> nimble::base::Result<RunResult>
{
  auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
  if (!acceptor.ok()) {
    return acceptor.status();
  }
  const auto port = acceptor.value().port();
  const auto total_iterations = options.iterations + options.warmup;
  const auto server_tls = MakeServerTls(options);
  const auto client_tls = MakeClientTls(options);

  std::promise<nimble::base::Status> server_result;
  auto server_future = server_result.get_future();
  std::jthread server_thread(
    [acceptor_socket = std::move(acceptor).value(), &server_result, total_iterations, use_tls, server_tls]() mutable {
      auto accepted = acceptor_socket.Accept(std::chrono::seconds(5));
      if (!accepted.ok()) {
        server_result.set_value(accepted.status());
        return;
      }
      auto connection = nimble::transport::TransportConnection::FromAcceptedTcp(
        std::move(accepted).value(), std::chrono::seconds(5), use_tls ? &server_tls : nullptr);
      if (!connection.ok()) {
        server_result.set_value(connection.status());
        return;
      }
      auto transport = std::move(connection).value();
      for (std::uint32_t index = 0; index < total_iterations; ++index) {
        auto frame = transport.ReceiveFrameView(std::chrono::seconds(5));
        if (!frame.ok()) {
          server_result.set_value(frame.status());
          return;
        }
        auto status = transport.Send(frame.value(), std::chrono::seconds(5));
        if (!status.ok()) {
          server_result.set_value(status);
          return;
        }
      }
      transport.Close();
      server_result.set_value(nimble::base::Status::Ok());
    });

  const auto connect_start = Now();
  auto connection = nimble::transport::TransportConnection::Connect(
    "127.0.0.1", port, std::chrono::seconds(5), use_tls ? &client_tls : nullptr);
  const auto connect_end = Now();
  if (!connection.ok()) {
    return connection.status();
  }
  auto transport = std::move(connection).value();
  const auto tls_info = transport.tls_session_info();

  const auto frame = EncodeFixFrame("35=0|34=1|49=BUY|56=SELL|52=20260425-00:00:00.000|");
  std::vector<std::uint64_t> rtts;
  rtts.reserve(options.iterations);
  for (std::uint32_t index = 0; index < total_iterations; ++index) {
    const auto start = Now();
    auto status = SendFrame(transport, frame, options.gather_send);
    if (!status.ok()) {
      return status;
    }
    auto echo = transport.ReceiveFrameView(std::chrono::seconds(5));
    if (!echo.ok()) {
      return echo.status();
    }
    const auto end = Now();
    if (index >= options.warmup) {
      rtts.push_back(DurationNs(start, end));
    }
  }
  transport.Close();

  auto server_status = server_future.get();
  if (!server_status.ok()) {
    return server_status;
  }

  return RunResult{
    .label = use_tls ? "tls-transport-rtt" : "tcp-transport-rtt",
    .connect_ns = DurationNs(connect_start, connect_end),
    .rtt = Compute(std::move(rtts)),
    .tls_info = tls_info,
  };
}

auto
ParseOptions(int argc, char** argv) -> Options
{
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    auto next = [&]() -> std::string_view {
      if (index + 1 >= argc) {
        return {};
      }
      ++index;
      return argv[index];
    };
    if (arg == "--iterations") {
      options.iterations = static_cast<std::uint32_t>(std::stoul(std::string(next())));
    } else if (arg == "--warmup") {
      options.warmup = static_cast<std::uint32_t>(std::stoul(std::string(next())));
    } else if (arg == "--cert") {
      options.certificate_chain_file = std::string(next());
    } else if (arg == "--key") {
      options.private_key_file = std::string(next());
    } else if (arg == "--ca") {
      options.ca_file = std::string(next());
    } else if (arg == "--contiguous-send") {
      options.gather_send = false;
    }
  }
  return options;
}

auto
HasTlsFiles(const Options& options) -> bool
{
  return !options.certificate_chain_file.empty() && !options.private_key_file.empty() && !options.ca_file.empty();
}

void
PrintResult(const RunResult& result)
{
  const auto& stats = result.rtt;
  std::cout << result.label << "\n";
  std::cout << "  connect_or_handshake_ns=" << result.connect_ns << "\n";
  if (result.tls_info.has_value()) {
    std::cout << "  tls_protocol=" << result.tls_info->protocol << "\n";
    std::cout << "  tls_cipher=" << result.tls_info->cipher << "\n";
    std::cout << "  tls_session_reused=" << (result.tls_info->session_reused ? "true" : "false") << "\n";
  }
  std::cout << "  rtt_count=" << stats.count << " avg_ns=" << static_cast<std::uint64_t>(stats.avg_ns)
            << " min_ns=" << stats.min_ns << " p50_ns=" << stats.p50_ns << " p95_ns=" << stats.p95_ns
            << " p99_ns=" << stats.p99_ns << " max_ns=" << stats.max_ns << "\n";
}

} // namespace

int
main(int argc, char** argv)
{
  const auto options = ParseOptions(argc, argv);
  auto tcp = RunEcho(false, options);
  if (!tcp.ok()) {
    std::cerr << "tcp baseline failed: " << tcp.status().message() << '\n';
    return 1;
  }
  PrintResult(tcp.value());

  if (!HasTlsFiles(options)) {
    std::cout << "tls-transport-rtt skipped: pass --cert, --key, and --ca\n";
    return 0;
  }
  if (!nimble::runtime::TlsTransportEnabledAtBuild()) {
    std::cout << "tls-transport-rtt skipped: binary was built without optional TLS support\n";
    return 0;
  }

  auto tls = RunEcho(true, options);
  if (!tls.ok()) {
    std::cerr << "tls baseline failed: " << tls.status().message() << '\n';
    return 1;
  }
  PrintResult(tls.value());
  return 0;
}
