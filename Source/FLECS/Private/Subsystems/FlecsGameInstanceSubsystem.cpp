#include "Subsystems/FlecsGameInstanceSubsystem.h"
#include "Engine/World.h"

void UFlecsGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bIsShuttingDown = false;
	EcsWorld = MakeUnique<flecs::world>();
}

void UFlecsGameInstanceSubsystem::Deinitialize()
{
	bIsShuttingDown = true;
	bIsProgressing = false;
	ActiveWorld.Reset();
	DestroyActiveWorldScope();

	for (TPair<FName, flecs::entity>& pair : PersistentSystems)
	{
		if (EcsWorld.IsValid() && EcsWorld->is_alive(pair.Value.id()))
		{
			pair.Value.destruct();
		}
	}
	PersistentSystems.Empty();
	EcsWorld.Reset();

	Super::Deinitialize();
}

flecs::world* UFlecsGameInstanceSubsystem::GetEcsWorld()
{
	return EcsWorld.Get();
}

const flecs::world* UFlecsGameInstanceSubsystem::GetEcsWorld() const
{
	return EcsWorld.Get();
}

flecs::entity UFlecsGameInstanceSubsystem::GetActiveWorldScope() const
{
	return ActiveWorldScope;
}

bool UFlecsGameInstanceSubsystem::AttachWorld(UWorld& InWorld)
{
	if (bIsShuttingDown || !EcsWorld.IsValid())
	{
		return false;
	}

	if (ActiveWorld.Get() == &InWorld && ActiveWorldScope.is_valid())
	{
		return true;
	}

	DestroyActiveWorldScope();
	ActiveWorld = &InWorld;
	LastProgressFrame = MAX_uint64;

	const FString scopeName = FString::Printf(TEXT("UnrealWorldScope_%llu"), ++WorldGeneration);
	FTCHARToUTF8 convertedName(*scopeName);
	ActiveWorldScope = EcsWorld->entity(convertedName.Get());
	return ActiveWorldScope.is_valid();
}

void UFlecsGameInstanceSubsystem::DetachWorld(UWorld& InWorld)
{
	if (ActiveWorld.Get() != &InWorld)
	{
		return;
	}

	ActiveWorld.Reset();
	LastProgressFrame = MAX_uint64;
	DestroyActiveWorldScope();
}

bool UFlecsGameInstanceSubsystem::ProgressFromWorld(UWorld& InWorld, float DeltaTime)
{
	if (bIsShuttingDown || bIsProgressing || DeltaTime <= 0.0f || ActiveWorld.Get() != &InWorld || !EcsWorld.IsValid())
	{
		return false;
	}

	if (LastProgressFrame == GFrameCounter)
	{
		return false;
	}

	LastProgressFrame = GFrameCounter;
	bIsProgressing = true;
	EcsWorld->progress(static_cast<ecs_ftime_t>(DeltaTime));
	bIsProgressing = false;
	return true;
}

FName UFlecsGameInstanceSubsystem::MakeWorldSystemName(FName InSystemName) const
{
	return FName(*FString::Printf(TEXT("World_%llu_%s"), WorldGeneration, *InSystemName.ToString()));
}

flecs::entity UFlecsGameInstanceSubsystem::CreatePersistentEntity(const char* InName)
{
	return EcsWorld.IsValid() ? EcsWorld->entity(InName) : flecs::entity();
}

flecs::entity UFlecsGameInstanceSubsystem::CreateWorldEntity(const char* InName)
{
	if (!EcsWorld.IsValid() || !ActiveWorldScope.is_valid())
	{
		return flecs::entity();
	}

	return EcsWorld->entity(InName).child_of(ActiveWorldScope);
}

bool UFlecsGameInstanceSubsystem::UnregisterPersistentSystem(FName SystemName)
{
	flecs::entity* foundSystem = PersistentSystems.Find(SystemName);
	if (!foundSystem)
	{
		return false;
	}

	if (EcsWorld.IsValid() && EcsWorld->is_alive(foundSystem->id()))
	{
		foundSystem->destruct();
	}

	PersistentSystems.Remove(SystemName);
	return true;
}

bool UFlecsGameInstanceSubsystem::UnregisterPersistentSystem(flecs::entity SystemEntity)
{
	if (!EcsWorld.IsValid() || !SystemEntity.is_valid() || SystemEntity.world().c_ptr() != EcsWorld->c_ptr())
	{
		return false;
	}

	for (const TPair<FName, flecs::entity>& pair : PersistentSystems)
	{
		if (pair.Value == SystemEntity)
		{
			return UnregisterPersistentSystem(pair.Key);
		}
	}

	return false;
}

void UFlecsGameInstanceSubsystem::DestroyActiveWorldScope()
{
	if (EcsWorld.IsValid() && ActiveWorldScope.is_valid() && EcsWorld->is_alive(ActiveWorldScope.id()))
	{
		ActiveWorldScope.destruct();
	}

	ActiveWorldScope = flecs::entity();
}
