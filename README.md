# flECS Unreal Engine 5.7 Plugin

![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.x-black?logo=unrealengine)
![Platform](https://img.shields.io/badge/Platform-Win64-blue)
![Linking](https://img.shields.io/badge/Linking-Static%20Library-success)
![flecs](https://img.shields.io/badge/flecs-4.1.6-orange)

A lightweight Unreal Engine plugin that allows using the **flecs** ECS framework inside UE gameplay code.

This is an integration plugin only. It provides Unreal-side wiring (module + subsystem + build integration) so you can use flecs in Unreal projects.

## What This Plugin Is

- A bridge between Unreal Engine and flecs.
- A `UGameInstanceSubsystem` (`UFlecsGameInstanceSubsystem`) that owns a worker session for the active Unreal world.
- A `UWorldSubsystem` (`UFlecsSubsystem`) bridge that attaches and detaches the active world session.
- Runtime registration and unregistration of flecs systems.

## Current Support / Constraints

- flecs version: **4.1.5**
- Supported platform: **Win64 only**
- Linking mode: **static library** (`flecs_static.lib`)
- Intended scope: Unreal plugin integration, not a custom ECS fork

## Third-Party Source

- Original flecs repository: [GitHub](https://github.com/sandermertens/flecs)
## Credits

- flecs author: [Sander Mertens](https://github.com/SanderMertens)
- Unreal plugin integration: [Wellsaik](https://github.com/ArtemIyX)

## Why flecs

flecs provides:

- Fast and portable zero dependency C++ API
- Modern type-safe C++17 API that doesn't use STL containers
- Full support for Entity Relationships
- Fast native support for hierarchies and prefabs
- Code base that builds quickly
- Browser support via emscripten
- Cache-friendly archetype / SoA storage for very high entity throughput
- Automatic component registration across shared libraries / DLLs
- Free-function queries and automatic system execution
- Multi-core execution with a fast lockless scheduler

## Folder Layout (Expected)

```text
Plugins/FLECS/
  Source/
    FLECS/
      FLECS.Build.cs
      Public/
      Private/
      ThirdParty/
        flecs/
          Include/
          Lib/
            Win64/
              flecs_static.lib
```

## Basic Usage (C++)

Example: get the subsystem, access world, register a simple OnUpdate system.

```cpp
// ExampleActor.cpp
#include "Subsystems/FlecsSubsystem.h"
#include "GameFramework/Actor.h"

namespace DemoEcs
{
	struct FMoveSpeed
	{
		float Value;
	};

	struct FPosition
	{
		float X;
	};
}

void AExampleActor::BeginPlay()
{
	Super::BeginPlay();

	if (!GetWorld())
	{
		return;
	}

	UFlecsSubsystem* FlecsSubsystem = GetWorld()->GetSubsystem<UFlecsSubsystem>();
	if (!FlecsSubsystem)
	{
		return;
	}

	flecs::entity Mover = FlecsSubsystem->CreateWorldEntity("Mover");
	if (!Mover.is_valid())
	{
		return;
	}

	Mover
		.set<DemoEcs::FPosition>({0.0f})
		.set<DemoEcs::FMoveSpeed>({50.0f});

	// Register (or replace) runtime system
	FlecsSubsystem->RegisterOnUpdateSystem<DemoEcs::FPosition, const DemoEcs::FMoveSpeed>(
		TEXT("MoveSystem"),
		[](DemoEcs::FPosition& Position, const DemoEcs::FMoveSpeed& Speed)
		{
			Position.X += Speed.Value;
		});
}
```

To remove the runtime system later:

```cpp
if (UFlecsSubsystem* FlecsSubsystem = GetWorld()->GetSubsystem<UFlecsSubsystem>())
{
	FlecsSubsystem->UnregisterSystem(TEXT("MoveSystem"));
}
```

## Tick Phase Configuration

The subsystem tick phase is configurable via plugin Developer Settings:

- `Pre Physics`
- `During Physics`
- `Post Physics`
- `Post Update Work`

Path: `Project Settings -> Plugins -> FLECS`

## Notes

- The Flecs world is GameInstance-scoped and survives level travel. Use `CreateWorldEntity` for map-local entities and `CreatePersistentEntity` for cross-level entities.
- The world bridge is limited to gameplay worlds and advances Flecs at most once per Unreal frame. Zero or negative deltas are ignored.
- GameInstances, including PIE instances, own isolated Flecs worlds. GameInstance subsystems do not replicate.
- Persistence across process shutdown still requires SaveGame serialization.
- If you update flecs version or platform support, update `FLECS.Build.cs`

## License

[MIT](LICENSE)
## Simulation ownership

The GameInstance subsystem attaches one simulation session to the active Unreal
world. Each session runs on a dedicated worker thread, which creates, mutates,
and destroys its Flecs world. Unreal world attach and detach notifications remain
on the game thread; detach wakes and joins the worker before its session ends.

The worker advances at a fixed `1/60` second step, independent of Unreal frame
rate and time dilation. Pause is explicit through `SetSimulationPaused`; queued
commands still drain while paused. A slow step may cause at most one immediate
catch-up step; deadlines more than one step behind are skipped and counted. The
fixed delta is never adjusted. This is a best-effort clock, not a real-time
guarantee.

Submit bounded commands through `SubmitWorkerCommand`, or register systems via
the subsystem registration helpers. These callbacks execute on the simulation
worker. They must not capture or access UObjects, the game thread, or Unreal
network objects. The queue rejects work after its 1,024-command capacity and
reports rejection count, depth, and oldest queued-command age in the immutable
simulation publication. The publication also reports epoch, tick, step time,
maximum step time, deadline lateness, and missed deadlines. Do not retain Flecs
entities, iterators, or pointers outside a worker callback.
