#include "Subsystems/FlecsGameInstanceSubsystem.h"
#include "Engine/World.h"

#include <atomic>

namespace
{
	std::atomic<uint64> NextWorldSerial{ 0 };
}

void UFlecsGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bIsShuttingDown = false;
	EcsWorld = MakeUnique<flecs::world>();
	WorldSerial = ++NextWorldSerial;
}

void UFlecsGameInstanceSubsystem::Deinitialize()
{
	bIsShuttingDown = true;
	bIsProgressing = false;
	ActiveWorld.Reset();
	DestroyActiveWorldScope();
	DestroyNetworkAccountScope();

	for (TPair<FName, flecs::entity>& pair : PersistentSystems)
	{
		if (EcsWorld.IsValid() && EcsWorld->is_alive(pair.Value.id()))
		{
			pair.Value.destruct();
		}
	}
	PersistentSystems.Empty();
	EcsWorld.Reset();
	WorldSerial = 0;

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

flecs::entity UFlecsGameInstanceSubsystem::GetNetworkAccountScope() const
{
	return NetworkAccountScope;
}

flecs::entity UFlecsGameInstanceSubsystem::GetNetworkGameScope() const
{
	return NetworkGameScope;
}

FFlecsEntityHandle UFlecsGameInstanceSubsystem::MakeEntityHandle(ecs_entity_t InEntity) const
{
	if (!EcsWorld.IsValid() || !EcsWorld->is_alive(InEntity))
	{
		return FFlecsEntityHandle();
	}

	return FFlecsEntityHandle(static_cast<uint64>(InEntity), WorldSerial);
}

bool UFlecsGameInstanceSubsystem::ResolveEntityHandle(
	const FFlecsEntityHandle& InHandle, ecs_entity_t& OutEntity) const
{
	if (!EcsWorld.IsValid() || !InHandle.IsValid() || WorldSerial == 0)
	{
		return false;
	}

	const uint64 EntityValue = InHandle.EntityId;
	if (InHandle.WorldSerial != WorldSerial || EntityValue == 0)
	{
		return false;
	}

	const ecs_entity_t Entity = static_cast<ecs_entity_t>(EntityValue);
	if (!EcsWorld->is_valid(Entity) || !EcsWorld->is_alive(Entity))
	{
		return false;
	}

	OutEntity = Entity;
	return true;
}

bool UFlecsGameInstanceSubsystem::IsEntityInScope(ecs_entity_t InEntity, flecs::entity InScope) const
{
	if (!EcsWorld.IsValid() || !InScope.is_valid() || !EcsWorld->is_alive(InEntity))
	{
		return false;
	}

	flecs::entity Current(EcsWorld->c_ptr(), InEntity);
	while (Current.is_valid())
	{
		if (Current.id() == InScope.id())
		{
			return true;
		}

		const flecs::entity Parent = Current.parent();
		if (!Parent.is_valid() || Parent.id() == Current.id())
		{
			break;
		}
		Current = Parent;
	}

	return false;
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
	NetworkGameScope = flecs::entity();
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

flecs::entity UFlecsGameInstanceSubsystem::CreateAccountEntity(const char* InName)
{
	if (!EcsWorld.IsValid() || !NetworkAccountScope.is_valid())
	{
		return flecs::entity();
	}

	return EcsWorld->entity(InName).child_of(NetworkAccountScope);
}

flecs::entity UFlecsGameInstanceSubsystem::CreateWorldEntity(const char* InName)
{
	return CreateNetworkGameEntity(InName);
}

flecs::entity UFlecsGameInstanceSubsystem::CreateNetworkGameEntity(const char* InName)
{
	if (!EcsWorld.IsValid() || !NetworkGameScope.is_valid())
	{
		return flecs::entity();
	}

	return EcsWorld->entity(InName).child_of(NetworkGameScope);
}

flecs::entity UFlecsGameInstanceSubsystem::CreateNetworkAccountScope()
{
	DestroyNetworkAccountScope();
	if (!EcsWorld.IsValid())
	{
		return flecs::entity();
	}

	NetworkAccountScope = EcsWorld->entity("NetworkAccountScope");
	return NetworkAccountScope;
}

flecs::entity UFlecsGameInstanceSubsystem::CreateNetworkGameScope()
{
	DestroyNetworkGameScope();
	if (!EcsWorld.IsValid() || !ActiveWorldScope.is_valid())
	{
		return flecs::entity();
	}

	NetworkGameScope = EcsWorld->entity("NetworkGameScope").child_of(ActiveWorldScope);
	return NetworkGameScope;
}

void UFlecsGameInstanceSubsystem::DestroyNetworkAccountScope()
{
	if (EcsWorld.IsValid() && NetworkAccountScope.is_valid() &&
		EcsWorld->is_alive(NetworkAccountScope.id()))
	{
		NetworkAccountScope.destruct();
	}
	NetworkAccountScope = flecs::entity();
}

void UFlecsGameInstanceSubsystem::DestroyNetworkGameScope()
{
	if (EcsWorld.IsValid() && NetworkGameScope.is_valid() &&
		EcsWorld->is_alive(NetworkGameScope.id()))
	{
		NetworkGameScope.destruct();
	}
	NetworkGameScope = flecs::entity();
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
	DestroyNetworkGameScope();
	if (EcsWorld.IsValid() && ActiveWorldScope.is_valid() && EcsWorld->is_alive(ActiveWorldScope.id()))
	{
		ActiveWorldScope.destruct();
	}

	ActiveWorldScope = flecs::entity();
}
