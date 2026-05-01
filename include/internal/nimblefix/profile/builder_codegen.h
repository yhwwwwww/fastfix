#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::profile {

auto
GenerateCppApiHeader(const NormalizedDictionary& dictionary, std::string_view namespace_name = {})
  -> base::Result<std::string>;

auto
WriteCppApiHeader(const std::filesystem::path& path,
                  const NormalizedDictionary& dictionary,
                  std::string_view namespace_name = {}) -> base::Status;

} // namespace nimble::profile
