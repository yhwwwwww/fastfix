#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/profile_registry.h"
#include "nimblefix/runtime/sharded_runtime.h"
#include "nimblefix/runtime/trace.h"
#include "nimblefix/session/session_key.h"

namespace nimble::runtime {

inline constexpr std::uint64_t kFirstDynamicSessionId = 0x8000'0000'0000'0000ULL;

using SessionFactory = std::function<base::Result<CounterpartyConfig>(const session::SessionKey& key)>;

class WhitelistSessionFactory
{
public:
	void Allow(std::string_view begin_string, std::string_view sender_comp_id, const CounterpartyConfig& config_template);
	void AllowAny(const CounterpartyConfig& config_template);

	auto operator()(const session::SessionKey& key) const -> base::Result<CounterpartyConfig>;

private:
	struct Entry
	{
		std::string begin_string;
		std::string sender_comp_id;
		CounterpartyConfig config_template;
	};

	std::unordered_map<std::string, Entry> entries_;
	std::optional<CounterpartyConfig> allow_any_template_;
};

struct ResolvedCounterparty
{
	CounterpartyConfig counterparty;
	profile::NormalizedDictionaryView dictionary;
};

class Engine
{
public:
	Engine();
	~Engine();

	Engine(const Engine&) = delete;
	auto operator=(const Engine&) -> Engine& = delete;
	Engine(Engine&&) = delete;
	auto operator=(Engine&&) -> Engine& = delete;

	auto LoadProfiles(const EngineConfig& config) -> base::Status;
	auto Boot(const EngineConfig& config) -> base::Status;
	auto EnsureManagedQueueRunnerStarted(const void* owner,
																			 ApplicationCallbacks* application,
																			 std::optional<ManagedQueueApplicationRunnerOptions>* options) -> base::Status;
	auto StopManagedQueueRunner(const void* owner) -> base::Status;
	auto ReleaseManagedQueueRunner(const void* owner) -> base::Status;
	auto PollManagedQueueWorkerOnce(const void* owner, std::uint32_t worker_id) -> base::Result<std::size_t>;

	[[nodiscard]] auto profiles() const -> const ProfileRegistry&;
	[[nodiscard]] auto runtime() const -> const ShardedRuntime*;
	[[nodiscard]] auto mutable_runtime() -> ShardedRuntime*;
	[[nodiscard]] auto metrics() const -> const MetricsRegistry&;
	[[nodiscard]] auto mutable_metrics() -> MetricsRegistry*;
	[[nodiscard]] auto trace() const -> const TraceRecorder&;
	[[nodiscard]] auto mutable_trace() -> TraceRecorder*;
	[[nodiscard]] auto config() const -> const EngineConfig*;

	[[nodiscard]] auto FindCounterpartyConfig(std::uint64_t session_id) const -> const CounterpartyConfig*;
	[[nodiscard]] auto FindListenerConfig(std::string_view name) const -> const ListenerConfig*;
	auto LoadDictionaryView(std::uint64_t profile_id) const -> base::Result<profile::NormalizedDictionaryView>;
	auto ResolveInboundSession(const codec::SessionHeader& header) const -> base::Result<ResolvedCounterparty>;
	auto ResolveInboundSession(const codec::SessionHeaderView& header) const -> base::Result<ResolvedCounterparty>;

	void SetSessionFactory(SessionFactory factory);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace nimble::runtime
