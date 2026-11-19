# FLECS

The runtime integration between Unreal and the bundled Flecs ECS. It owns the simulation worker, attaches a Flecs world to an Unreal game instance/world, and provides bounded ways to send work to the worker and receive copied data back.

## Add to a game module

Add `FLECS` to the appropriate dependency list in your game module's `.Build.cs`. Include `Subsystems/FlecsSubsystem.h` for world-scoped system registration or `Subsystems/FlecsGameInstanceSubsystem.h` for worker commands, lifecycle callbacks, and publications.

The `UFlecsSubsystem` world subsystem attaches the game instance subsystem when a supported world is initialized. `UFlecsGameInstanceSubsystem` starts one `FlecsSimulation` runner for its active attached world. The Flecs world is created, progressed, and destroyed on that worker thread. The configured fixed rate defaults to 60 Hz; see **Project Settings → Plugins → FLECS → Simulation**. A changed rate is picked up when a world is attached.

## Register worker work

From the game thread, after the world is ready, register a world-scoped `OnUpdate` system:

```cpp
#include "Subsystems/FlecsSubsystem.h"

struct FExampleHealth { float Value = 100.0f; };

bool RegisterHealthSystem(UWorld& World)
{
    UFlecsSubsystem* Flecs = World.GetSubsystem<UFlecsSubsystem>();
    return Flecs && Flecs->RegisterOnUpdateSystem<FExampleHealth>(
        TEXT("Example.Health"),
        [](FExampleHealth& Health) { Health.Value = FMath::Max(0.0f, Health.Value); });
}
```

The return value indicates queue admission, not completion. `UnregisterSystem` is also queued. For game-instance registrations that should be reapplied on later world attachments, see `RegisterPersistentOnUpdateSystem`, `RegisterPersistentWorkerCommand`, and `RegisterPersistentWorkerLifecycle`.

Use `SubmitWorkerCommand(ExpectedEpoch, ...)` for one-off worker work. Obtain `ExpectedEpoch` from `GetActiveEpoch()` on the game thread and handle results such as `StaleEpoch`, `QueueFull`, and `Stopping`. An epoch identifies a **local world attachment**, not a network session or server epoch. Never retain Flecs world/entity references across worker calls or world detachments.

## Worker-to-game data

Register a named `RegisterPersistentDataPublisher` callback to extract data after a simulation step into `FFlecsSimulationDataBuilder`. `TryAdd` copies bytes into a bounded publication; inspect its result and keep publication payloads free of UObject and Flecs references. The limits are declared in `FlecsSimulationTypes.h` (16,384 items, 4 MiB total, and 64 KiB per item).

On the game thread, read immutable snapshots with `GetPublication()` and `GetDataPublication()`. Publications can be absent, replaced, or belong to an older local epoch, so check their epoch before using them. Remove named publishers and lifecycle registrations when they are no longer needed.

**Thread boundary:** worker callbacks and systems must not access or capture UObjects, actors, or Unreal connection state. Copy the necessary plain data across the boundary instead. `RegisterOnUpdateSystem` does not automatically replicate components; see [FLECSNetwork](../FLECSNetwork/README.md).

## Diagnostics

- `stat Flecs` shows worker progress, publication and queue counters, and sampled memory.
- `flecs.Diagnostics.SystemTime 1` enables Flecs system-time measurements; it adds per-system measurement overhead.
- `flecs.Diagnostics.WorldMemory 1` enables at-most-once-per-second Flecs world-memory sampling. The value is Flecs-reported memory, not total process memory.

See [FLECSNetwork diagnostics](../FLECSNetwork/README.md#diagnostics) for network counters and the viewport overlay. The core module links the bundled library through [FLECSThirdParty](../FLECSThirdParty/README.md); its native Flecs export list is curated, not the entire upstream API.
