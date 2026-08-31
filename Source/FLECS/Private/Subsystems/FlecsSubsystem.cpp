#include "Subsystems/FlecsSubsystem.h"

#include "Settings/FlecsDeveloperSettings.h"
#include "Engine/Level.h"
#include "Engine/GameInstance.h"

UFlecsSubsystem::UFlecsSubsystem() = default;

void UFlecsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	CachedOwnerSubsystem.Reset();
	UFlecsGameInstanceSubsystem* ownerSubsystem = nullptr;
	if (UWorld* world = GetWorld())
	{
		if (UGameInstance* gameInstance = world->GetGameInstance())
		{
			CachedOwnerSubsystem = gameInstance->GetSubsystem<UFlecsGameInstanceSubsystem>();
			ownerSubsystem = CachedOwnerSubsystem.Get();
		}
	}

	if (!ownerSubsystem)
	{
		return;
	}

	TickFunction.Owner = this;
	TickFunction.bCanEverTick = true;
	TickFunction.bStartWithTickEnabled = true;
	TickFunction.TickGroup = ResolveTickGroup();

	if (UWorld* world = GetWorld())
	{
		if (!ownerSubsystem->AttachWorld(*world))
		{
			return;
		}

		if (world->PersistentLevel)
		{
			TickFunction.RegisterTickFunction(world->PersistentLevel);
		}
	}
}

void UFlecsSubsystem::Deinitialize()
{
	if (TickFunction.IsTickFunctionRegistered())
	{
		TickFunction.UnRegisterTickFunction();
	}

	for (TPair<FName, flecs::entity>& pair : RuntimeSystems)
	{
		if (pair.Value.is_valid())
		{
			pair.Value.destruct();
		}
	}
	RuntimeSystems.Empty();

	if (UFlecsGameInstanceSubsystem* ownerSubsystem = GetOwnerSubsystem())
	{
		if (UWorld* world = GetWorld())
		{
			ownerSubsystem->DetachWorld(*world);
		}
	}
	CachedOwnerSubsystem.Reset();

	Super::Deinitialize();
}

bool UFlecsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GameRPC;
}

flecs::world* UFlecsSubsystem::GetEcsWorld()
{
	if (UFlecsGameInstanceSubsystem* ownerSubsystem = GetOwnerSubsystem())
	{
		return ownerSubsystem->GetEcsWorld();
	}
	return nullptr;
}

const flecs::world* UFlecsSubsystem::GetEcsWorld() const
{
	if (const UFlecsGameInstanceSubsystem* ownerSubsystem = GetOwnerSubsystem())
	{
		return ownerSubsystem->GetEcsWorld();
	}
	return nullptr;
}

flecs::entity UFlecsSubsystem::CreatePersistentEntity(const char* InName)
{
	return GetOwnerSubsystem() ? GetOwnerSubsystem()->CreatePersistentEntity(InName) : flecs::entity();
}

flecs::entity UFlecsSubsystem::CreateWorldEntity(const char* InName)
{
	return GetOwnerSubsystem() ? GetOwnerSubsystem()->CreateWorldEntity(InName) : flecs::entity();
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
	const flecs::world* ecsWorld = GetEcsWorld();
	if (!ecsWorld || !SystemEntity.is_valid() || SystemEntity.world().c_ptr() != ecsWorld->c_ptr())
	{
		return false;
	}

	FName foundName = NAME_None;
	for (const TPair<FName, flecs::entity>& pair : RuntimeSystems)
	{
		if (pair.Value == SystemEntity)
		{
			foundName = pair.Key;
			break;
		}
	}

	if (foundName != NAME_None)
	{
		SystemEntity.destruct();
		RuntimeSystems.Remove(foundName);
		return true;
	}

	return false;
}

void UFlecsSubsystem::Tick(float DeltaTime)
{
	if (UWorld* world = GetWorld())
	{
		if (UFlecsGameInstanceSubsystem* ownerSubsystem = GetOwnerSubsystem())
		{
			ownerSubsystem->ProgressFromWorld(*world, DeltaTime);
		}
	}
}

UFlecsGameInstanceSubsystem* UFlecsSubsystem::GetOwnerSubsystem() const
{
	if (UFlecsGameInstanceSubsystem* ownerSubsystem = CachedOwnerSubsystem.Get())
	{
		return ownerSubsystem;
	}

	UFlecsSubsystem* mutableThis = const_cast<UFlecsSubsystem*>(this);
	if (UWorld* world = mutableThis->GetWorld())
	{
		if (UGameInstance* gameInstance = world->GetGameInstance())
		{
			mutableThis->CachedOwnerSubsystem = gameInstance->GetSubsystem<UFlecsGameInstanceSubsystem>();
			return mutableThis->CachedOwnerSubsystem.Get();
		}
	}

	return nullptr;
}

ETickingGroup UFlecsSubsystem::ResolveTickGroup() const
{
	const UFlecsDeveloperSettings* settings = GetDefault<UFlecsDeveloperSettings>();
	switch (settings->TickPhase)
	{
	case EFlecsTickPhase::PrePhysics:
		return TG_PrePhysics;
	case EFlecsTickPhase::DuringPhysics:
		return TG_DuringPhysics;
	case EFlecsTickPhase::PostUpdateWork:
		return TG_PostUpdateWork;
	case EFlecsTickPhase::PostPhysics:
	default:
		return TG_PostPhysics;
	}
}

void UFlecsSubsystem::FFlecsTickFunction::ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	if (Owner)
	{
		Owner->Tick(DeltaTime);
	}
}

FString UFlecsSubsystem::FFlecsTickFunction::DiagnosticMessage()
{
	return TEXT("UFlecsSubsystem::FFlecsTickFunction");
}
