#include "fastfix/codec/raw_passthrough.h"
#include "fastfix/codec/fast_int_format.h"
#include "fastfix/codec/fix_tags.h"
#include "fastfix/codec/simd_scan.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace fastfix::codec {

namespace {

using namespace fastfix::codec::tags;

inline constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;

template <typename Integer>
inline constexpr std::size_t kIntBufSize =
    static_cast<std::size_t>(std::numeric_limits<Integer>::digits10) + 3U;

auto ParseUint32(std::string_view text) -> std::uint32_t {
    std::uint32_t value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return 0;
    }
    return value;
}

// Returns the string_view for a field value within the raw bytes,
// given the value_offset and value_length.
auto ValueView(std::span<const std::byte> data, std::size_t offset, std::size_t len) -> std::string_view {
    return std::string_view(reinterpret_cast<const char*>(data.data() + offset), len);
}

// Lightweight field scanner: finds tag=value\x01 and returns tag, value offset/len, next pos.
struct ScannedField {
    std::uint32_t tag{0};
    std::size_t value_offset{0};
    std::size_t value_length{0};
    std::size_t next_pos{0};
};

auto ScanField(std::span<const std::byte> data, std::size_t pos, std::byte delim) -> ScannedField {
    const auto eq_byte = static_cast<std::byte>('=');
    if (pos >= data.size()) {
        return {};
    }
    const auto remaining = data.size() - pos;
    const auto* soh = FindByte(data.data() + pos, remaining, delim);
    const auto soh_idx = static_cast<std::size_t>(soh - data.data());
    if (soh_idx >= data.size() || soh_idx == pos) {
        return {};
    }
    const auto field_len = soh_idx - pos;
    const auto* eq = FindByte(data.data() + pos, field_len, eq_byte);
    const auto eq_off = static_cast<std::size_t>(eq - (data.data() + pos));
    if (eq_off >= field_len || eq_off == 0) {
        return {};
    }
    const auto* tag_chars = reinterpret_cast<const char*>(data.data() + pos);
    std::uint32_t tag = 0;
    auto [ptr, ec] = std::from_chars(tag_chars, tag_chars + eq_off, tag);
    if (ec != std::errc() || ptr != tag_chars + eq_off) {
        return {};
    }
    return ScannedField{
        .tag = tag,
        .value_offset = pos + eq_off + 1,
        .value_length = field_len - eq_off - 1,
        .next_pos = soh_idx + 1,
    };
}

// Session-layer header tags — used to determine where raw_body begins.
// Matches CompiledMessageDecoder::is_header_tag() plus framing tags.
auto IsSessionHeaderTag(std::uint32_t tag) -> bool {
    return IsSessionEnvelopeTag(tag);
}

}  // namespace

auto DecodeRawPassThrough(
    std::span<const std::byte> data,
    char delimiter,
    bool verify_checksum) -> base::Result<RawPassThroughView> {
    const auto delim = static_cast<std::byte>(static_cast<unsigned char>(
        delimiter == '\0' ? kFixSoh : delimiter));

    RawPassThroughView view;
    view.raw_message = data;

    // Field 0: must be tag 8 (BeginString)
    auto f = ScanField(data, 0, delim);
    if (f.tag != kBeginString) {
        return base::Status::FormatError("FIX frame must begin with tag 8");
    }
    view.begin_string = ValueView(data, f.value_offset, f.value_length);

    // Field 1: must be tag 9 (BodyLength)
    f = ScanField(data, f.next_pos, delim);
    if (f.tag != kBodyLength) {
        return base::Status::FormatError("FIX frame must have tag 9 after tag 8");
    }
    const auto declared_body_length = ParseUint32(ValueView(data, f.value_offset, f.value_length));
    const auto body_start_offset = f.next_pos;

    // Locate the checksum field by scanning backwards from the end.
    // FIX checksum is always the last field, formatted as 10=NNN<delim> (7 bytes).
    if (data.size() < 7) {
        return base::Status::FormatError("FIX frame too short for CheckSum");
    }
    if (static_cast<std::byte>('1') != data[data.size() - 7] ||
        static_cast<std::byte>('0') != data[data.size() - 6] ||
        static_cast<std::byte>('=') != data[data.size() - 5] ||
        delim != data[data.size() - 1]) {
        return base::Status::FormatError("FIX frame missing CheckSum (tag 10)");
    }
    const auto checksum_field_start = data.size() - 7;

    if (verify_checksum) {
        std::uint32_t actual_sum = 0;
        for (std::size_t i = 0; i < checksum_field_start; ++i) {
            actual_sum += std::to_integer<unsigned char>(data[i]);
        }
        actual_sum %= 256U;
        const auto expected = ParseUint32(ValueView(data, data.size() - 4, 3));
        if (actual_sum != expected) {
            return base::Status::FormatError("CheckSum mismatch");
        }
    }

    // Validate BodyLength: region between body_start_offset and checksum_field_start.
    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (declared_body_length != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    // Scan header fields until we hit the first non-header (application) tag.
    // Header fields always precede body fields in a well-formed FIX message,
    // so we can stop scanning once we see the first application-level tag.
    std::size_t last_header_end = f.next_pos;
    std::size_t pos = f.next_pos;
    while (pos < checksum_field_start) {
        f = ScanField(data, pos, delim);
        if (f.tag == 0 && f.next_pos == 0) {
            return base::Status::FormatError("malformed FIX field");
        }

        if (!IsSessionHeaderTag(f.tag)) {
            // First non-header field: body starts here; stop scanning.
            break;
        }

        const auto val = ValueView(data, f.value_offset, f.value_length);
        switch (f.tag) {
            case kMsgType: view.msg_type = val; break;
            case kMsgSeqNum: view.msg_seq_num = ParseUint32(val); break;
            case kSenderCompID: view.sender_comp_id = val; break;
            case kTargetCompID: view.target_comp_id = val; break;
            case kSendingTime: view.sending_time = val; break;
            default: break;
        }
        last_header_end = f.next_pos;
        pos = f.next_pos;
    }

    // raw_body is everything between last_header_end and checksum_field_start.
    // This contains all application-level fields, unparsed.
    view.raw_body = data.subspan(last_header_end, checksum_field_start - last_header_end);
    view.valid = true;
    return view;
}

auto EncodeForwarded(
    const RawPassThroughView& inbound,
    const ForwardingOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (!inbound.valid) {
        return base::Status::InvalidArgument("inbound view is not valid");
    }

    const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;
    auto& out = buffer->storage;
    out.clear();

    // Reserve a reasonable capacity to avoid reallocations.
    out.reserve(inbound.raw_message.size() + 128);

    // 1. Write 8=<begin_string>SOH
    const auto begin_string = options.begin_string.empty()
        ? inbound.begin_string
        : options.begin_string;
    out.append(kBeginStringPrefix);
    out.append(begin_string);
    out.push_back(soh);

    // 2. Write 9= + placeholder + SOH
    out.append(kBodyLengthPrefix);
    const auto body_length_offset = out.size();
    out.append(kBodyLengthPlaceholderWidth, '0');
    out.push_back(soh);
    const auto body_start = out.size();

    // 3. Write 35=<msg_type>SOH
    out.append(kMsgTypePrefix);
    out.append(inbound.msg_type);
    out.push_back(soh);

    // 4. Write 49=<sender>SOH
    out.append(kSenderCompIDPrefix);
    out.append(options.sender_comp_id);
    out.push_back(soh);

    // 5. Write 56=<target>SOH
    out.append(kTargetCompIDPrefix);
    out.append(options.target_comp_id);
    out.push_back(soh);

    // 6. Write 34=<seq_num>SOH
    {
        char buf[10];
        const auto len = FormatUint32(buf, options.msg_seq_num);
        out.append(kMsgSeqNumPrefix);
        out.append(buf, len);
        out.push_back(soh);
    }

    // 7. Write 52=<sending_time>SOH
    out.append(kSendingTimePrefix);
    out.append(options.sending_time);
    out.push_back(soh);

    // 8. Optional: 43=Y SOH (PossDupFlag)
    if (options.poss_dup) {
        out.append(kPossDupFlagYesField);
        out.push_back(soh);
    }

    // 9. Optional: 122=<orig_sending_time>SOH
    if (!options.orig_sending_time.empty()) {
        out.append(kOrigSendingTimePrefix);
        out.append(options.orig_sending_time);
        out.push_back(soh);
    }

    // 10. Optional: 115=<on_behalf_of_comp_id>SOH
    if (!options.on_behalf_of_comp_id.empty()) {
        out.append(kOnBehalfOfCompIDPrefix);
        out.append(options.on_behalf_of_comp_id);
        out.push_back(soh);
    }

    // 9. Optional: 128=<deliver_to_comp_id>SOH
    if (!options.deliver_to_comp_id.empty()) {
        out.append(kDeliverToCompIDPrefix);
        out.append(options.deliver_to_comp_id);
        out.push_back(soh);
    }

    // 10. Splice raw body bytes
    if (!inbound.raw_body.empty()) {
        const auto* body_ptr = reinterpret_cast<const char*>(inbound.raw_body.data());
        out.append(body_ptr, inbound.raw_body.size());
    }

    // 11. Fill in BodyLength
    const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
    {
        char buf[10];
        const auto len = FormatUint32(buf, body_length);
        if (len > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError("BodyLength exceeds placeholder width");
        }
        out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
    }

    // 12. Compute and append 10=<checksum>SOH
    std::uint32_t checksum = 0;
    for (const auto ch : out) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    std::array<char, 3> cksum{};
    cksum[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    cksum[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    cksum[2] = static_cast<char>('0' + (checksum % 10U));
    out.append(kCheckSumPrefix);
    out.append(cksum.data(), 3);
    out.push_back(soh);

    return base::Status::Ok();
}

auto EncodeReplay(
    const RawPassThroughView& stored,
    const ReplayOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (!stored.valid) {
        return base::Status::InvalidArgument("stored view is not valid");
    }

    const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;
    auto& out = buffer->storage;
    out.clear();

    out.reserve(stored.raw_message.size() + 64);

    // 1. 8=<begin_string>SOH
    const auto begin_string = options.begin_string.empty()
        ? stored.begin_string
        : options.begin_string;
    out.append(kBeginStringPrefix);
    out.append(begin_string);
    out.push_back(soh);

    // 2. 9=<placeholder>SOH
    out.append(kBodyLengthPrefix);
    const auto body_length_offset = out.size();
    out.append(kBodyLengthPlaceholderWidth, '0');
    out.push_back(soh);
    const auto body_start = out.size();

    // 3. 35=<msg_type>SOH
    out.append(kMsgTypePrefix);
    out.append(stored.msg_type);
    out.push_back(soh);

    // 4. 49=<sender>SOH
    out.append(kSenderCompIDPrefix);
    out.append(options.sender_comp_id);
    out.push_back(soh);

    // 5. 56=<target>SOH
    out.append(kTargetCompIDPrefix);
    out.append(options.target_comp_id);
    out.push_back(soh);

    // 6. 34=<seq_num>SOH
    {
        char buf[10];
        const auto len = FormatUint32(buf, options.msg_seq_num);
        out.append(kMsgSeqNumPrefix);
        out.append(buf, len);
        out.push_back(soh);
    }

    // 7. 52=<sending_time>SOH
    out.append(kSendingTimePrefix);
    out.append(options.sending_time);
    out.push_back(soh);

    // 8. 43=Y SOH (always set for replay)
    out.append(kPossDupFlagYesField);
    out.push_back(soh);

    // 9. 122=<orig_sending_time>SOH
    if (!options.orig_sending_time.empty()) {
        out.append(kOrigSendingTimePrefix);
        out.append(options.orig_sending_time);
        out.push_back(soh);
    }

    // 10. Optional 1137=<default_appl_ver_id>SOH
    if (!options.default_appl_ver_id.empty()) {
        out.append(kDefaultApplVerIDPrefix);
        out.append(options.default_appl_ver_id);
        out.push_back(soh);
    }

    // 11. Splice raw body bytes unchanged
    if (!stored.raw_body.empty()) {
        const auto* body_ptr = reinterpret_cast<const char*>(stored.raw_body.data());
        out.append(body_ptr, stored.raw_body.size());
    }

    // 12. Backfill BodyLength
    const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
    {
        char buf[10];
        const auto len = FormatUint32(buf, body_length);
        if (len > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError("BodyLength exceeds placeholder width");
        }
        out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
    }

    // 13. Compute and append 10=<checksum>SOH
    std::uint32_t checksum = 0;
    for (const auto ch : out) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    std::array<char, 3> cksum{};
    cksum[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    cksum[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    cksum[2] = static_cast<char>('0' + (checksum % 10U));
    out.append(kCheckSumPrefix);
    out.append(cksum.data(), 3);
    out.push_back(soh);

    return base::Status::Ok();
}

// ComputeChecksumSIMD is now in simd_scan.h (fastfix::codec namespace).

auto EncodeReplayInto(
    const RawPassThroughView& stored,
    const ReplayOptions& options,
    session::EncodedFrameBytes* out) -> base::Status {
    if (out == nullptr) {
        return base::Status::InvalidArgument("output is null");
    }
    if (!stored.valid) {
        return base::Status::InvalidArgument("stored view is not valid");
    }

    const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;

    // Estimate buffer size: header + trailer, plus body if not zero-copy
    const auto estimated_size = 128U + options.sender_comp_id.size() +
        options.target_comp_id.size() + options.begin_string.size() +
        options.default_appl_ver_id.size() + options.sending_time.size() +
        options.orig_sending_time.size() + stored.msg_type.size() +
        (options.zero_copy_body ? 0U : stored.raw_body.size());

    // Choose storage: inline or overflow
    char* buf;
    if (estimated_size <= session::kEncodedFrameInlineCapacity) {
        buf = reinterpret_cast<char*>(out->inline_storage.data());
        out->overflow_storage.clear();
    } else {
        out->inline_size = 0;
        out->overflow_storage.resize(estimated_size + 64);
        buf = reinterpret_cast<char*>(out->overflow_storage.data());
    }

    // Incremental checksum — accumulate as we write each byte of the header.
    // FIX checksum = sum of ALL bytes before the 10= field (including 8=, 9=).
    std::uint32_t header_checksum = 0;
    std::size_t pos = 0;

    auto append_sv = [&](std::string_view sv) {
        std::memcpy(buf + pos, sv.data(), sv.size());
        for (std::size_t i = 0; i < sv.size(); ++i) {
            header_checksum += static_cast<unsigned char>(sv[i]);
        }
        pos += sv.size();
    };
    auto append_char = [&](char ch) {
        buf[pos++] = ch;
        header_checksum += static_cast<unsigned char>(ch);
    };

    // 1. 8=<begin_string>SOH
    const auto begin_string = options.begin_string.empty()
        ? stored.begin_string : options.begin_string;
    append_sv(kBeginStringPrefix);
    append_sv(begin_string);
    append_char(soh);

    // 2. 9=<placeholder>SOH — write "9=" tracked, then placeholder untracked (will adjust later)
    append_sv(kBodyLengthPrefix);
    const auto body_length_offset = pos;
    // Write placeholder without tracking — we'll add the real digits' sum after backfill
    std::memcpy(buf + pos, "0000000", 7);
    pos += 7;
    append_char(soh);
    const auto body_start = pos;

    // 3. 35=<msg_type>SOH
    append_sv(kMsgTypePrefix);
    append_sv(stored.msg_type);
    append_char(soh);

    // 4. 49=<sender>SOH
    append_sv(kSenderCompIDPrefix);
    append_sv(options.sender_comp_id);
    append_char(soh);

    // 5. 56=<target>SOH
    append_sv(kTargetCompIDPrefix);
    append_sv(options.target_comp_id);
    append_char(soh);

    // 6. 34=<seq_num>SOH
    {
        char num_buf[10];
        const auto num_len = FormatUint32(num_buf, options.msg_seq_num);
        append_sv(kMsgSeqNumPrefix);
        append_sv(std::string_view(num_buf, num_len));
        append_char(soh);
    }

    // 7. 52=<sending_time>SOH
    append_sv(kSendingTimePrefix);
    append_sv(options.sending_time);
    append_char(soh);

    // 8. 43=Y SOH
    append_sv(kPossDupFlagYesField);
    append_char(soh);

    // 9. 122=<orig_sending_time>SOH
    if (!options.orig_sending_time.empty()) {
        append_sv(kOrigSendingTimePrefix);
        append_sv(options.orig_sending_time);
        append_char(soh);
    }

    // 10. Optional 1137=<default_appl_ver_id>SOH
    if (!options.default_appl_ver_id.empty()) {
        append_sv(kDefaultApplVerIDPrefix);
        append_sv(options.default_appl_ver_id);
        append_char(soh);
    }

    // 11. Body handling: zero-copy scatter-gather or inline copy
    std::uint32_t body_checksum = 0;
    const auto body_data_size = stored.raw_body.size();
    if (body_data_size > 0) {
        body_checksum = ComputeChecksumSIMD(
            reinterpret_cast<const char*>(stored.raw_body.data()),
            body_data_size);
    }

    std::size_t splice_offset = 0;
    if (options.zero_copy_body) {
        // Zero-copy path: body stays at its source address (e.g., mmap).
        // Record splice point; trailer written next in buf.
        splice_offset = pos;
    } else {
        // Inline path: copy body into buf.
        if (body_data_size > 0) {
            std::memcpy(buf + pos, stored.raw_body.data(), body_data_size);
            pos += body_data_size;
        }
    }

    // 12. Backfill BodyLength with zero-padded 7-digit format and add its digit sum.
    const auto body_length = options.zero_copy_body
        ? static_cast<std::uint32_t>(pos - body_start + body_data_size)
        : static_cast<std::uint32_t>(pos - body_start);
    {
        char bl_buf[7];
        bl_buf[0] = '0' + static_cast<char>((body_length / 1000000U) % 10U);
        bl_buf[1] = '0' + static_cast<char>((body_length / 100000U) % 10U);
        bl_buf[2] = '0' + static_cast<char>((body_length / 10000U) % 10U);
        bl_buf[3] = '0' + static_cast<char>((body_length / 1000U) % 10U);
        bl_buf[4] = '0' + static_cast<char>((body_length / 100U) % 10U);
        bl_buf[5] = '0' + static_cast<char>((body_length / 10U) % 10U);
        bl_buf[6] = '0' + static_cast<char>(body_length % 10U);
        std::memcpy(buf + body_length_offset, bl_buf, 7);
        // Add the actual digit bytes to the checksum (placeholder was not tracked)
        for (int i = 0; i < 7; ++i) {
            header_checksum += static_cast<unsigned char>(bl_buf[i]);
        }
    }

    // 13. Combine: header (incremental) + body (SIMD), no full-buffer rescan
    std::uint32_t checksum = (header_checksum + body_checksum) % 256U;

    std::memcpy(buf + pos, kCheckSumPrefix.data(), kCheckSumPrefix.size());
    pos += kCheckSumPrefix.size();
    buf[pos++] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    buf[pos++] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    buf[pos++] = static_cast<char>('0' + (checksum % 10U));
    buf[pos++] = soh;

    // Finalize
    if (estimated_size <= session::kEncodedFrameInlineCapacity) {
        out->inline_size = pos;
    } else {
        out->overflow_storage.resize(pos);
    }

    if (options.zero_copy_body && body_data_size > 0) {
        out->external_body = stored.raw_body;
        out->body_splice_offset = splice_offset;
    } else {
        out->external_body = {};
        out->body_splice_offset = 0;
    }

    return base::Status::Ok();
}

}  // namespace fastfix::codec
