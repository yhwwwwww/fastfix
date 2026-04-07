// libFuzzer entry point for codec mutation-based fuzzing.
//
// Build:
//   xmake build fastfix-fuzz-codec-libfuzzer
//
// Run:
//   ./build/linux/x86_64/release/fastfix-fuzz-codec-libfuzzer corpus_dir/
//
// The entry point feeds arbitrary bytes through PeekSessionHeaderView() and
// DecodeFixMessageView() to find crashes, hangs, or assertion failures in the
// codec parsing paths.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"

namespace {

// Lazily initialized dictionary shared across all fuzz iterations.
struct FuzzState {
    fastfix::profile::LoadedProfile profile;
    std::optional<fastfix::profile::NormalizedDictionaryView> dictionary;
    bool initialized{false};
};

auto GetFuzzState() -> FuzzState& {
    static FuzzState state;
    if (!state.initialized) {
        // Build a minimal dictionary inline — no external artifact file needed.
        fastfix::profile::NormalizedDictionary dict;
        dict.profile_id = 9001U;
        dict.schema_hash = 0x9001900190019001ULL;
        dict.fields = {
            {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
            {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
            {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
            {11U, "ClOrdID", fastfix::profile::ValueType::kString, 0U},
            {55U, "Symbol", fastfix::profile::ValueType::kString, 0U},
            {552U, "NoSides", fastfix::profile::ValueType::kInt, 0U},
            {54U, "Side", fastfix::profile::ValueType::kChar, 0U},
            {453U, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U},
            {448U, "PartyID", fastfix::profile::ValueType::kString, 0U},
            {98U, "EncryptMethod", fastfix::profile::ValueType::kInt, 0U},
            {108U, "HeartBtInt", fastfix::profile::ValueType::kInt, 0U},
        };
        dict.messages = {
            fastfix::profile::MessageDef{
                .msg_type = "D",
                .name = "NewOrderSingle",
                .field_rules = {
                    {35U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                    {11U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                    {55U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                    {552U, 0U},
                },
                .flags = 0U,
            },
            fastfix::profile::MessageDef{
                .msg_type = "0",
                .name = "Heartbeat",
                .field_rules = {},
                .flags = 0U,
            },
            fastfix::profile::MessageDef{
                .msg_type = "A",
                .name = "Logon",
                .field_rules = {},
                .flags = 0U,
            },
        };
        dict.groups = {
            fastfix::profile::GroupDef{
                .count_tag = 552U,
                .delimiter_tag = 54U,
                .name = "Sides",
                .field_rules = {
                    {54U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                    {453U, 0U},
                },
                .flags = 0U,
            },
            fastfix::profile::GroupDef{
                .count_tag = 453U,
                .delimiter_tag = 448U,
                .name = "Parties",
                .field_rules = {
                    {448U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                },
                .flags = 0U,
            },
        };

        auto artifact = fastfix::profile::BuildProfileArtifact(dict);
        if (!artifact.ok()) {
            __builtin_trap();
        }
        const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-fuzz-codec-libfuzzer.art";
        auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
        if (!write_status.ok()) {
            __builtin_trap();
        }
        auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
        std::filesystem::remove(artifact_path);
        if (!loaded.ok()) {
            __builtin_trap();
        }
        state.profile = std::move(loaded).value();
        auto dv = fastfix::profile::NormalizedDictionaryView::FromProfile(state.profile);
        if (!dv.ok()) {
            __builtin_trap();
        }
        state.dictionary.emplace(std::move(dv).value());
        state.initialized = true;
    }
    return state;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto& fuzz = GetFuzzState();
    if (!fuzz.initialized) {
        return 0;
    }

    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

    // Exercise the peek path (zero-copy header scanner)
    (void)fastfix::codec::PeekSessionHeaderView(bytes);

    // Exercise the full decode-view path (parsed field slots + groups)
    (void)fastfix::codec::DecodeFixMessageView(bytes, *fuzz.dictionary);

    // Exercise the owning decode path
    std::vector<std::byte> owned(bytes.begin(), bytes.end());
    (void)fastfix::codec::DecodeFixMessage(owned, *fuzz.dictionary);

    return 0;
}
