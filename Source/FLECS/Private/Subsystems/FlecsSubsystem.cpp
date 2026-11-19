#include "Subsystems/FlecsSubsystem.h"

#include "Engine/World.h"
#include "Engine/GameInstance.h"

void UFlecsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	CachedOwnerSubsystem.Reset();
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			CachedOwnerSubsystem = GameInstance->GetSubsystem<UFlecsGameInstanceSubsystem>();
			if (UFlecsGameInstanceSubsystem* Owner = CachedOwnerSubsystem.Get())
			{
				Owner->AttachWorld(*World);
			}
		}
	}
}

void UFlecsSubsystem::Deinitialize()
{
	if (UFlecsGameInstanceSubsystem* Owner = GetOwnerSubsystem())
	{
		if (UWorld* World = GetWorld())
		{
			Owner->DetachWorld(*World);
		}
	}
	CachedOwnerSubsystem.Reset();
	Super::Deinitialize();
}

bool UFlecsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GameRPC;
}

bool UFlecsSubsystem::UnregisterSystem(FName SystemName)
{
	UFlecsGameInstanceSubsystem* Owner = GetOwnerSubsystem();
	if (!Owner || SystemName.IsNone())
	{
		return false;
	}
	const FName ScopedName = Owner->MakeWorldSystemName(SystemName);
	FTCHARToUTF8 ConvertedName(*ScopedName.ToString());
	const std::string WorkerName(ConvertedName.Get());
	return Owner->SubmitWorkerCommand([WorkerName](flecs::world& World)
	{
		flecs::entity Existing = World.lookup(WorkerName.c_str());
		if (Existing.is_alive())
		{
			Existing.destruct();
		}
	});
}

UFlecsGameInstanceSubsystem* UFlecsSubsystem::GetOwnerSubsystem() const
{
	if (UFlecsGameInstanceSubsystem* Owner = CachedOwnerSubsystem.Get())
	{
		return Owner;
	}
	const_cast<UFlecsSubsystem*>(this)->CachedOwnerSubsystem.Reset();
	if (UWorld* World = GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			UFlecsGameInstanceSubsystem* Owner = GameInstance->GetSubsystem<UFlecsGameInstanceSubsystem>();
			const_cast<UFlecsSubsystem*>(this)->CachedOwnerSubsystem = Owner;
			return Owner;
		}
	}
	return nullptr;
}
