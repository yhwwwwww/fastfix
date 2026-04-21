#pragma once

#include <cstdint>

namespace nimble::runtime {

enum class IoBackend : std::uint32_t
{
  kEpoll = 0,
  kIoUring = 1,
};

// Returns the best available backend for the running kernel.
auto DetectBestIoBackend() -> IoBackend;

// Returns true if the given backend is supported on this system.
auto IsIoBackendAvailable(IoBackend backend) -> bool;

} // namespace nimble::runtime
