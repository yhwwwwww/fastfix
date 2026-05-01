#pragma once

#include <cstdint>
#include <utility>

#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::runtime {

template<class Profile>
class ProfileBinding
{
public:
  explicit ProfileBinding(profile::NormalizedDictionaryView dictionary)
    : dictionary_(std::move(dictionary))
  {
  }

  [[nodiscard]] auto dictionary() const -> const profile::NormalizedDictionaryView& { return dictionary_; }
  [[nodiscard]] auto dispatcher() const -> const typename Profile::Dispatcher& { return dispatcher_; }
  [[nodiscard]] auto profile_id() const -> std::uint64_t { return Profile::kProfileId; }
  [[nodiscard]] auto schema_hash() const -> std::uint64_t { return Profile::kSchemaHash; }

private:
  profile::NormalizedDictionaryView dictionary_;
  typename Profile::Dispatcher dispatcher_{};
};

} // namespace nimble::runtime
