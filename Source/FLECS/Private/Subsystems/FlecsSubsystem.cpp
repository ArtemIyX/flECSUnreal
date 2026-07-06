#include "Subsystems/FlecsSubsystem.h"

#include "Settings/FlecsDeveloperSettings.h"
#include "Engine/Level.h"


UFlecsSubsystem::UFlecsSubsystem()
{
	EcsWorld.Reset();
}

void UFlecsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	EcsWorld = MakeUnique<flecs::world>();

	TickFunction.Owner = this;
	TickFunction.bCanEverTick = true;
	TickFunction.bStartWithTickEnabled = true;
	TickFunction.TickGroup = ResolveTickGroup();

	if (UWorld* World = GetWorld())
	{
		if (World->PersistentLevel)
		{
			TickFunction.RegisterTickFunction(World->PersistentLevel);
		}
	}
}

void UFlecsSubsystem::Deinitialize()
{
	if (TickFunction.IsTickFunctionRegistered())
	{
		TickFunction.UnRegisterTickFunction();
	}

	for (TPair<FName, flecs::entity>& Pair : RuntimeSystems)
	{
		if (Pair.Value.is_valid())
		{
			Pair.Value.destruct();
		}
	}
	RuntimeSystems.Empty();

	EcsWorld.Reset();

	Super::Deinitialize();
}

bool UFlecsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::GameRPC;
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
	flecs::entity* FoundSystem = RuntimeSystems.Find(SystemName);
	if (!FoundSystem)
	{
		return false;
	}

	if (FoundSystem->is_valid())
	{
		FoundSystem->destruct();
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

	FName FoundName = NAME_None;
	for (const TPair<FName, flecs::entity>& Pair : RuntimeSystems)
	{
		if (Pair.Value == SystemEntity)
		{
			FoundName = Pair.Key;
			break;
		}
	}

	SystemEntity.destruct();

	if (FoundName != NAME_None)
	{
		RuntimeSystems.Remove(FoundName);
	}

	return true;
}

void UFlecsSubsystem::Tick(float DeltaTime)
{
	if (EcsWorld.IsValid())
	{
		EcsWorld->progress(static_cast<ecs_ftime_t>(DeltaTime));
	}
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
