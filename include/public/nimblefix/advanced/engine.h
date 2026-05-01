#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/advanced/runtime_application.h"

namespace nimble::runtime {

class Engine;

// Advanced queue-runner controls for untyped live runtime consumers. Typed
// applications should stay on runtime::Initiator<Profile>/Acceptor<Profile> and
// do not need these hooks directly.
auto EnsureManagedQueueRunnerStarted(Engine& engine,
                                     const void* owner,
                                     ApplicationCallbacks* application,
                                     std::optional<ManagedQueueApplicationRunnerOptions>* options) -> base::Status;
auto StopManagedQueueRunner(Engine& engine, const void* owner) -> base::Status;
auto ReleaseManagedQueueRunner(Engine& engine, const void* owner) -> base::Status;
auto PollManagedQueueWorkerOnce(Engine& engine, const void* owner, std::uint32_t worker_id)
  -> base::Result<std::size_t>;

} // namespace nimble::runtime
