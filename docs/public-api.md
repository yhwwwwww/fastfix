# NimbleFIX Public API Guide

NimbleFIX now keeps exported headers and repository-private headers in separate trees:

- `include/public/nimblefix/`: headers intended for library consumers
- `include/internal/nimblefix/`: implementation-only headers used by NimbleFIX itself, tests, and internal tools

External applications should add only `include/public/` to the compiler include path.

The important change is that the heavy runtime/session entry points now expose standalone public declarations and hide their private state in implementation files. In particular, these headers no longer publish internal worker, poller, store, or protocol state layouts:

- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/live_acceptor.h`
- `nimblefix/runtime/live_initiator.h`
- `nimblefix/session/admin_protocol.h`

The earlier `_api.h` compatibility names are gone. The canonical header names above are now the real exported API surface.

## What External Users Should Include

### Primary Runtime API

Use these headers for normal application bring-up:

- `nimblefix/runtime/application.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/io_backend.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/live_acceptor.h`
- `nimblefix/runtime/live_initiator.h`

### Session And Message API

Use these when integrating custom session flows, building messages, or consuming decoded messages:

- `nimblefix/session/admin_protocol.h`
- `nimblefix/session/session_core.h`
- `nimblefix/session/session_handle.h`
- `nimblefix/session/session_key.h`
- `nimblefix/session/session_send_envelope.h`
- `nimblefix/session/encoded_frame.h`
- `nimblefix/session/resend_recovery.h`
- `nimblefix/message/message_builder.h`
- `nimblefix/message/message_view.h`
- `nimblefix/message/typed_message_view.h`
- `nimblefix/codec/fix_codec.h`
- `nimblefix/codec/fix_tags.h`
- `nimblefix/codec/raw_passthrough.h`

### Profiles, Stores, And Transport Configuration

Use these when loading dictionaries or selecting persistence/transport policies:

- `nimblefix/profile/profile_loader.h`
- `nimblefix/profile/normalized_dictionary.h`
- `nimblefix/store/session_store.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`
- `nimblefix/transport/tcp_transport.h`

### Metrics And Advanced Runtime Introspection

These remain public because application code and tests legitimately use them:

- `nimblefix/runtime/metrics.h`
- `nimblefix/runtime/trace.h`
- `nimblefix/runtime/profile_registry.h`
- `nimblefix/runtime/sharded_runtime.h`
- `nimblefix/base/status.h`
- `nimblefix/base/result.h`

## Exported But Not Recommended As Direct User Surface

Not every header under `include/public/` is a first include for downstream code. Most applications only need the primary runtime headers plus the specific session, message, profile, or store types they actively use. The remaining public headers stay exported because public signatures depend on them or because they are legitimate low-level extension points.

Treat these as support headers rather than the primary integration surface:

- `nimblefix/base/inline_split_vector.h`
- `nimblefix/codec/compiled_decoder.h`
- `nimblefix/message/fixed_layout_writer.h`
- `nimblefix/message/message_ref.h`
- `nimblefix/message/message_structs.h`
- `nimblefix/profile/artifact.h`
- `nimblefix/session/transport_profile.h`
- `nimblefix/session/validation_policy.h`
- `nimblefix/session/session_snapshot.h`
- `nimblefix/session/encoded_application_message.h`

The old runtime support headers `config_io.h`, `io_poller.h`, `live_session_registry.h`, `shard_poller.h`, `poll_wakeup.h`, and `timer_wheel.h` are no longer part of the installed public surface.

## Not For External Consumers

Anything that exists only under `include/internal/nimblefix/` is repository-private and should not be included by downstream code. Examples:

- `nimblefix/base/spsc_queue.h`
- `nimblefix/codec/fast_int_format.h`
- `nimblefix/codec/simd_scan.h`
- `nimblefix/profile/builder_codegen.h`
- `nimblefix/profile/dictgen_input.h`
- `nimblefix/profile/overlay.h`
- `nimblefix/profile/artifact_builder.h`
- `nimblefix/runtime/live_session_worker.h`
- `nimblefix/runtime/interop_harness.h`
- `nimblefix/runtime/soak.h`

If a future external API really needs one of those types, it should be promoted deliberately into the public tree instead of being included from `include/internal/`.
