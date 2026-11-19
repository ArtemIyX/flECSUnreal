# FLECSModding

A runtime registry for trusted native component schemas, entity definitions, commands, manifests, and peer-specific wire mappings. Despite the module name, it does not load arbitrary scripts or provide a sandbox. Your game is responsible for loading and registering trusted code before the session registry freezes.

## Build a registry

Include `ModRegistry/ModSchemaRegistry.h` and use `FlecsModRegistry::FRegistry`:

1. Register `FComponentSchema` entries with a namespaced lowercase stable ID such as `example:health`, a version, a maximum encoded size, and `Extract`, `Apply`, and `Remove` callbacks.
2. Register each `FEntityDefinition`, listing its allowed component IDs, maximum initial payload bytes, and entity creation callback.
3. Register `FCommandSchema` entries where needed, including a permission ID, payload limit, and execution callback. Optional `Validate` callbacks are preflight checks and must not mutate state.
4. Call `Freeze(ProtocolVersion, Error)` once setup is complete. Inspect the returned error; registration and freezing can fail on invalid, duplicate, missing, or over-limit metadata.

The network subsystem normally calls the game-provided session configurator before it freezes the registry. Its `FFlecsNetworkSessionRegistrar` exposes the registry and trusted spatial, permission, interest, and peer-spawn registration hooks. See [FLECSNetwork](../FLECSNetwork/README.md).

## Schemas and compatibility

`Extract`, `Apply`, `Remove`, `Create`, `Validate`, and `Execute` are trusted native callbacks invoked by worker-owned model code. Entity tokens are opaque and local to the worker: do not transmit them, use them as network identities, retain them beyond their valid lifetime, or access UObjects or connection state from these callbacks.

A component's visibility, update rate, priority, version, payload bound, compatibility metadata, and allowed definitions contribute to its session metadata. Use explicit, bounded codecs and change the schema version when the wire representation changes. Components not registered with the network model, or not included in a registered component payload, are not automatically replicated.

After freeze, `ExportManifestDescriptor` and `CompareRequired` compare complete manifests. A successful comparison supplies an `FSessionMapping` for that peer; use it for compact wire IDs rather than treating a local Flecs ID as a network ID. The registry is immutable after freeze. Limits such as maximum IDs, entries, and payload sizes are declared in `ModSchemaRegistry.h`; the transport may impose stricter limits.

The game project contains a native example under `Source/NetworkEntityExample/Private/ExampleMod`. Its worker state and codecs are examples, not a general-purpose mod loader.
