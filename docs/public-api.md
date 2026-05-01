# NimbleFIX Public API Guide

NimbleFIX exports two header trees:

- `include/public/nimblefix/`: supported external API
- `include/internal/nimblefix/`: repository-private implementation headers

External applications should add only `include/public/` to the include path.

## First Includes

The main application path is generated-first:

- your generated profile header such as `fix44_api.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/profile_binding.h`
- `nimblefix/runtime/initiator.h` or `nimblefix/runtime/acceptor.h`

Bring in these additional headers only when you need them:

- `nimblefix/runtime/application.h`
- `nimblefix/advanced/runtime_application.h`
- `nimblefix/advanced/engine.h`
- `nimblefix/advanced/session_handle.h`
- `nimblefix/session/session_send_envelope.h`
- `nimblefix/advanced/message_builder.h`
- `nimblefix/message/message_view.h`
- `nimblefix/profile/profile_loader.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`

## Lifecycle Contract

`Engine::Boot()` is the normal entry point. It validates `EngineConfig`, loads profiles from `profile_artifacts` and `profile_dictionaries`, optionally loads matching contract sidecars from `profile_contracts`, registers static counterparties, and makes `config()`, `profiles()`, `runtime()`, `FindCounterpartyConfig()`, and `FindListenerConfig()` available on success.

`Engine::LoadProfiles()` exists for tooling and tests that need profile loading without a booted runtime. Typed `Engine::Bind<Profile>()` works after either `LoadProfiles()` or `Boot()`, but normal applications should still prefer `Boot()`.

The expected startup order is:

1. Fill `EngineConfig`.
2. Call `ValidateEngineConfig()` if you want an explicit preflight step.
3. Call `Engine::Boot()`.
4. Call `engine.Bind<GeneratedProfile>()`.
5. Construct typed `runtime::Initiator<Profile>` or `runtime::Acceptor<Profile>`.
6. Open sessions or listeners.
7. Call `Run()`.

## Minimal Initiator Walkthrough

This is the shortest normal path from config to a running initiator on the typed generated API:

```cpp
#include "fix44_api.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/initiator.h"
#include "nimblefix/runtime/profile_binding.h"

class BuySideApp final : public nimble::generated::profile_4400::Handler {
public:
  auto OnSessionActive(nimble::runtime::Session<Profile>& session) -> nimble::base::Status override {
    NewOrderSingle order;
    order.cl_ord_id("ORD-001")
      .symbol("AAPL")
      .side(Side::Buy)
      .transact_time("20260429-09:30:00.000")
      .order_qty(100)
      .ord_type(OrdType::Limit)
      .price(150.25);
    return session.send(std::move(order));
  }

  auto OnExecutionReport(nimble::runtime::InlineSession<Profile>&,
                         ExecutionReportView exec) -> nimble::base::Status override {
    auto exec_id = exec.exec_id_raw();
    auto ord_status = exec.ord_status();
    (void)exec_id;
    (void)ord_status;
    return nimble::base::Status::Ok();
  }
};

nimble::runtime::EngineConfig config;
config.profile_artifacts.push_back("fix44.nfa");
config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
  .name = "buy-side",
  .session = {
    .session_id = 1001U,
    .key = nimble::session::SessionKey::ForInitiator("BUY1", "SELL1"),
    .profile_id = Profile::kProfileId,
    .heartbeat_interval_seconds = 30U,
    .is_initiator = true,
  },
  .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
  .reconnect_enabled = true,
  .reconnect_initial_ms = nimble::runtime::kDefaultReconnectInitialMs,
  .reconnect_max_ms = nimble::runtime::kDefaultReconnectMaxMs,
  .reconnect_max_retries = nimble::runtime::kUnlimitedReconnectRetries,
});

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
  return boot;
}

auto binding = engine.Bind<Profile>();
if (!binding.ok()) {
  return binding.status();
}

auto app = std::make_shared<BuySideApp>();
nimble::runtime::Initiator<Profile> initiator(&engine, &binding.value(), { .application = app });

auto open = initiator.OpenSession(1001U, "127.0.0.1", 9876);
if (!open.ok()) {
  return open;
}

return initiator.Run();
```

Notes:

- `Engine::Bind<Profile>()` validates both `profile_id` and `schema_hash` against the loaded artifact.
- `runtime::Session<Profile>` is the ordinary-thread send surface.
- `runtime::InlineSession<Profile>` is the inline callback surface for typed inbound dispatch.
- `OpenSession()` may block until the TCP dial succeeds or times out.
- `OpenSessionAsync()` defers the dial onto the runtime worker loop if the caller cannot block.

## Typed Session Boundaries

The generated-first runtime surface expresses send context with types:

- `runtime::Session<Profile>`: ordinary application-thread handle, owning send only.
- `runtime::InlineSession<Profile>`: direct runtime callback handle for typed inbound dispatch.

For first-time integrations, treat `OnSessionActive()` as the safe point to send the first application message.

Managed queue-runner control is also an advanced surface: include `nimblefix/advanced/engine.h` when untyped live runtimes or tests need `EnsureManagedQueueRunnerStarted(...)`, `StopManagedQueueRunner(...)`, `ReleaseManagedQueueRunner(...)`, or `PollManagedQueueWorkerOnce(...)`.

Advanced/raw send rules still matter only when you intentionally drop below the typed runtime surface:

- `SessionHandle::Send(message::MessageRef::Copy/Take(...))` and `SendEncoded(EncodedApplicationMessageRef::Take(...))` use the session command queue and require a single producer thread per handle.
- Borrowed `SessionHandle::Send(...)` / `SendEncoded(...)` variants remain advanced APIs; the typed surface intentionally does not expose them as the main business send contract.
- Queue-drained application threads must use owned send variants, not inline-borrowed sends.

## Minimal Acceptor Walkthrough

```cpp
#include "fix44_api.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/profile_binding.h"

class SellSideApp final : public nimble::generated::profile_4400::Handler {
public:
  auto OnNewOrderSingle(nimble::runtime::InlineSession<Profile>& session,
                        NewOrderSingleView order) -> nimble::base::Status override {
    ExecutionReport report;
    report.order_id("ORD-001")
      .exec_id("EXEC-001")
      .exec_type(ExecType::New)
      .ord_status(OrdStatus::New)
      .side(order.side().value())
      .leaves_qty(order.order_qty().value_or(0))
      .cum_qty(0)
      .avg_px(0.0);
    if (auto cl_ord_id = order.cl_ord_id(); cl_ord_id.has_value()) {
      report.cl_ord_id(*cl_ord_id);
    }
    if (auto symbol = order.symbol(); symbol.has_value()) {
      report.symbol(*symbol);
    }
    return session.send(std::move(report));
  }
};

nimble::runtime::EngineConfig config;
config.profile_artifacts.push_back("fix44.nfa");
config.listeners.push_back(nimble::runtime::ListenerConfig{
  .name = "main",
  .host = "0.0.0.0",
  .port = 9876,
});
config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
  .name = "sell-side",
  .session = {
    .session_id = 2001U,
    .key = nimble::session::SessionKey::ForAcceptor("SELL1", "BUY1"),
    .profile_id = Profile::kProfileId,
    .heartbeat_interval_seconds = 30U,
    .is_initiator = false,
  },
  .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
});

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
  return boot;
}

auto binding = engine.Bind<Profile>();
if (!binding.ok()) {
  return binding.status();
}

auto app = std::make_shared<SellSideApp>();
nimble::runtime::Acceptor<Profile> acceptor(&engine, &binding.value(), { .application = app });

auto open = acceptor.OpenListeners("main");
if (!open.ok()) {
  return open;
}

return acceptor.Run();
```

## Dynamic Acceptor Walkthrough

Unknown inbound Logons are not dynamic by default. The rules are:

- `accept_unknown_sessions = false`: unknown Logons are rejected and `SessionFactory` is ignored.
- `accept_unknown_sessions = true` and no factory installed: unknown Logons are still rejected.
- `accept_unknown_sessions = true` and a factory installed: static counterparties match first, then the factory is called for unknown Logons.

Dynamic onboarding contract:

- The factory input `SessionKey` is normalized to the local engine's perspective.
- `key.sender_comp_id` is the local acceptor CompID from inbound `TargetCompID(56)`.
- `key.target_comp_id` is the remote initiator CompID from inbound `SenderCompID(49)`.
- `listener` name, local port, and remote address are not part of the callback.
- Returning `session.session_id == 0` asks `Engine` to auto-assign a dynamic id from `kFirstDynamicSessionId` upward.
- `WhitelistSessionFactory::Allow(begin_string, local_sender_comp_id, template)` matches the same local-perspective key.

## Minimum Required Config

For a static initiator counterparty, the minimum meaningful fields are:

- `CounterpartyConfig::name`
- `session.session_id`
- `session.key.sender_comp_id`
- `session.key.target_comp_id`
- `session.profile_id`
- `transport_profile`
- `session.is_initiator = true`

For a static acceptor counterparty, the minimum meaningful fields are:

- `CounterpartyConfig::name`
- `session.session_id`
- `session.key.sender_comp_id` as the local acceptor CompID
- `session.key.target_comp_id` as the expected remote initiator CompID
- `session.profile_id`
- `session.is_initiator = false`

Additional conditional requirements:

- FIXT.1.1 sessions require `default_appl_ver_id`.
- `StoreMode::kMmap` and `StoreMode::kDurableBatch` require `store_path`.
- `RecoveryMode::kWarmRestart` requires a persistent store mode.
- `EngineConfig::listeners` is required before `runtime::Acceptor<Profile>::OpenListeners()`.

## Support Headers

These headers remain public because public signatures depend on them or advanced integrations use them directly:

- `nimblefix/base/result.h`
- `nimblefix/base/status.h`
- `nimblefix/runtime/application.h`
- `nimblefix/advanced/runtime_application.h`
- `nimblefix/runtime/acceptor.h`
- `nimblefix/runtime/initiator.h`
- `nimblefix/runtime/metrics.h`
- `nimblefix/runtime/profile_registry.h`
- `nimblefix/runtime/session.h`
- `nimblefix/runtime/profile_binding.h`
- `nimblefix/runtime/sharded_runtime.h`
- `nimblefix/runtime/trace.h`
- `nimblefix/advanced/encoded_application_message.h`
- `nimblefix/session/session_snapshot.h`
- `nimblefix/session/transport_profile.h`
- `nimblefix/session/validation_policy.h`

Everything under `include/internal/nimblefix/` remains repository-private.

## Do Not Do This

- Do not write new primary examples around `RuntimeEvent.message_view()` or `ApplicationCallbacks` when a generated typed handler is available.
- Do not skip `Engine::Bind<Profile>()`; it is the schema-match guard between generated code and loaded runtime artifacts.
- Do not share one `Session<Profile>` or raw `SessionHandle` send path across multiple producer threads. The runtime send path is single-producer.
- Do not default to inline-borrowed raw send variants unless you are intentionally using the advanced low-level surface.
- Do not assume `kBound` means the session is ready for business traffic. Wait for `OnSessionActive()` unless you intentionally need pre-activation behavior.
- Do not call `Engine::LoadProfiles()` and then treat `Boot()` as a no-op. `Boot()` reloads profiles and resets runtime state.
- Do not assume `accept_unknown_sessions = true` is enough by itself. Unknown Logons still need a `SessionFactory`.
