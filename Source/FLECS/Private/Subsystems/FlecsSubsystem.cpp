#include "Subsystems/FlecsSubsystem.h"

UFlecsSubsystem::UFlecsSubsystem()
{
	
}
UFlecsSubsystem::~UFlecsSubsystem()
{
	
}

void UFlecsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	EcsWorld = MakeUnique<flecs::world>();

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UFlecsSubsystem::Tick));
}

void UFlecsSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	for (TPair<FName, flecs::entity>& pair : RuntimeSystems)
	{
		if (pair.Value.is_valid())
		{
			pair.Value.destruct();
		}
	}
	RuntimeSystems.Empty();

	EcsWorld.Reset();

	Super::Deinitialize();
}

flecs::world* UFlecsSubsystem::GetEcsWorld()
{
	return EcsWorld.Get();
}

const flecs::world* UFlecsSubsystem::GetEcsWorld() const
{
	return EcsWorld.Get();
}

bool UFlecsSubsystem::UnregisterSystem(FName SystemName)
{
	flecs::entity* foundSystem = RuntimeSystems.Find(SystemName);
	if (!foundSystem)
	{
		return false;
	}

	if (foundSystem->is_valid())
	{
		foundSystem->destruct();
	}

	RuntimeSystems.Remove(SystemName);
	return true;
}

bool UFlecsSubsystem::UnregisterSystem(flecs::entity SystemEntity)
{
	if (!SystemEntity.is_valid())
	{
		return false;
	}

	FName foundName = NAME_None;
	for (const TPair<FName, flecs::entity>& Pair : RuntimeSystems)
	{
		if (Pair.Value == SystemEntity)
		{
			foundName = Pair.Key;
			break;
		}
	}

	SystemEntity.destruct();

	if (foundName != NAME_None)
	{
		RuntimeSystems.Remove(foundName);
	}

	return true;
}

bool UFlecsSubsystem::Tick(float DeltaTime)
{
	if (EcsWorld.IsValid())
	{
		EcsWorld->progress(static_cast<ecs_ftime_t>(DeltaTime));
	}

	return true;
}
