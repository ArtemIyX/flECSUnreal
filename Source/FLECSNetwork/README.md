# FLECSNetwork

The runtime bridge between the worker-owned Flecs entity model and Unreal Iris. The server owns networked entity identities and authoritative state; compatible peers receive bounded, relevance-selected state through owner-only `AFlecsReplicationChannel` actors. Standalone worlds use the authoritative model locally without needing a network connection.

## Setup

Enable the FLECS and Iris plugins and add `FLECSNetwork` to your game module's dependencies. If game code uses the core worker or registry types directly, add the corresponding `FLECS` and `FLECSModding` dependencies too.

Before world creation, install `UFlecsNetworkSessionSubsystem::SetDefaultConfigurator` with a protocol version. The game-thread configurator receives a `UWorld` and `FFlecsNetworkSessionRegistrar`. Register trusted schemas, entity definitions, commands and permission rules, spatial definitions, and any optional interest provider or peer-spawn factory before registry freeze. Clear the default configurator when your module shuts down. The configurator is process-wide; create per-world state inside it.

A peer is not ready for state until its manifest handshake succeeds. Observe `OnSessionEvent`, `GetPeerSessions`, and `IsPeerCompatible` for session state. `QueuePeerEntitySpawn` queues a peer-owned entity; `QueueSharedEntitySpawn` queues an unowned authoritative entity. Queue success is only admission: the corresponding `PeerSpawnFinished` or `SharedSpawnFinished` event reports the worker result and identity. Check errors and completion events rather than assuming creation succeeded.

The host project demonstrates configuration and spawning in `Source/NetworkEntityExample/NetworkEntityExample.cpp` and `Private/MovementExample`. See [FLECSModding](../FLECSModding/README.md) for schema registration and worker callback rules.

## Replication flow

1. The server model creates or changes networked entities on the Flecs worker. Schema `Extract` callbacks provide bounded component bytes; `Apply` and `Remove` apply received state on the client worker. `RefreshAuthoritativeComponentChanges` detects encoded changes to registered components. Direct Flecs components do not replicate automatically.
2. Worker publications provide copied spatial and identity data. The game-thread session selects entities per connection using relevance, visibility, distance tiers, component rates, and record/byte budgets.
3. The channel transfers bounded state records using an Iris fast array. Complete client batches are queued to the worker and acknowledged after application. The server tick heartbeat is tracked separately from state batches.
4. When an entity leaves relevance, its client replica is removed; this differs from authoritative despawn. If it becomes relevant again, it can be recreated with current state.

Use `FWorkerEntityModel` authoritative operations for network identity, spawn, component lifecycle, ownership, and despawn. Its methods are worker-thread-only. Local Flecs IDs and worker entity tokens must never be used as network identities. Commands arriving from clients require server-side permission and validation; their payloads and identities are not trusted.

A spatial definition can require a worker-safe position accessor, be always relevant without a position, or be skipped without one. By default the scheduler uses the player controller's focal location. **Project Settings → Plugins → FLECS → Networking** configures up to four increasing distance/rate tiers; these control replication eligibility, not simulation Hz. A custom per-peer interest provider, component rate cap, visibility, hysteresis, and budget can change the actual outcome.

**Networking → Limits** sets the authoritative network entity cap (1–100,000, default 5,000), per-peer owned entity cap (default 256), and the default per-peer selection budgets (128 records and 64 KiB). Values are captured when the network session subsystem initializes. The per-peer interest provider can override the selection budgets within the transport limits. Invalid configured values are clamped and logged; the per-peer ownership limit cannot exceed the entity cap. A rejected spawn reports `LimitReached`, while work exceeding a selection budget is deferred. A single update larger than the byte budget cannot be selected until its size or budget changes. The byte budget excludes Iris framing and packet overhead and is not a wire-bandwidth limit. Local Flecs entities are not counted against the authoritative network entity cap.

The per-peer relevant entity limit defaults to 5,000 and cannot exceed the session entity cap. The world cap does not guarantee that every entity can be replicated to a single peer; retained per-peer baselines are separately bounded to 1,000,000 components and 64 MiB of payload bytes. The scheduler defers additional admissions until an existing entity's exit has been acknowledged. Server and client session entity caps must match: the versioned session manifest envelope rejects mismatched capacities and older, unversioned envelopes.

## Codecs and quantization

`Quantization/FlecsQuantization.h` provides signed fixed-step and 16-bit circular-angle quantization plus network-order integer readers/writers. Helpers reject invalid values and report failure; callers own payload format, bounds, versioning, and canonical decoding. The host movement example uses signed centimeter values for location and a 16-bit yaw. Health uses its own codec; stamina and speed are local to the server example.

## Diagnostics

- `stat FlecsNetwork` displays selection/transfer cycles, relevance counters, logical payload counters, and tracked channel buffers.
- `flecs.Diagnostics.Overlay 1` enables the local viewport overlay. It displays per-world data; dedicated servers use stat groups instead.
- Payload counters are **logical payload bytes**, not measured wire bandwidth. They exclude Iris framing and packet overhead; state retries are deduplicated by logical batch and command retries by sequence. Memory counters cover the tracked buffers, not all Unreal networking allocations.
- Core worker timing and opt-in world-memory sampling are described in [FLECS](../FLECS/README.md#diagnostics).

The example debug actor's distance color and displayed rate are local configured-tier estimates. They are not proof of the server's selected tier or actual delivery rate, especially with custom interest providers or differing client settings.

Spatial metadata and complete live identities are copied on the simulation worker and handed to the game thread in bounded acknowledged chunks. Relevance uses only a complete snapshot from one generation; a slow game thread applies backpressure. The server tick heartbeat is copied separately and can advance while a snapshot is in flight. Large capacities can increase memory use and snapshot latency and should be measured in the target game. Retained metadata and container overhead are additional to the baseline payload limit.

`stat FlecsNetwork` and the plugin overlay report spatial snapshot container allocations separately from channel buffers. This is a partial figure: it excludes strings, transient workspaces, allocator overhead, Flecs world memory, and other process memory. It is sampled for each local world approximately once per second.
