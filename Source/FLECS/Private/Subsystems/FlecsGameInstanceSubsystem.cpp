#include "Subsystems/FlecsGameInstanceSubsystem.h"
#include "FlecsSimulationRunner.h"

#include "Engine/World.h"

#include <atomic>

namespace
{
	std::atomic<uint64> NextWorldSerial{ 0 };
	std::atomic<uint64> NextWorldEpoch{ 0 };
}

void UFlecsGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bIsShuttingDown = false;
	WorldSerial = ++NextWorldSerial;
}

void UFlecsGameInstanceSubsystem::Deinitialize()
{
	bIsShuttingDown = true;
	if (UWorld* World = ActiveWorld.Get())
	{
		OnWorldAboutToDetach.Broadcast(*World);
	}
	ActiveWorld.Reset();
	StopRunner();
	WorldSerial = 0;
	Super::Deinitialize();
}

bool UFlecsGameInstanceSubsystem::AttachWorld(UWorld& InWorld)
{
	check(IsInGameThread());
	if (bIsShuttingDown)
	{
		return false;
	}
	if (ActiveWorld.Get() == &InWorld && Runner.IsValid())
	{
		return true;
	}
	if (UWorld* PreviousWorld = ActiveWorld.Get())
	{
		OnWorldAboutToDetach.Broadcast(*PreviousWorld);
	}
	ActiveWorld.Reset();
	StopRunner();
	ActiveWorld = &InWorld;
	++WorldGeneration;
	Runner = MakeShared<FFlecsSimulationRunner, ESPMode::ThreadSafe>(++NextWorldEpoch);
	TArray<FFlecsWorkerCommand> StartupCommands;
	StartupCommands.Reserve(PersistentWorkerCommands.Num());
	for (const TPair<FName, FFlecsWorkerCommand>& Entry : PersistentWorkerCommands)
	{
		StartupCommands.Add(Entry.Value);
	}
	if (!Runner->Start(MoveTemp(StartupCommands)))
	{
		UE_LOG(LogTemp, Error, TEXT("FLECS simulation worker failed to initialize; the world will not advance."));
		Runner.Reset();
		ActiveWorld.Reset();
		return false;
	}
	OnWorldAttached.Broadcast(InWorld);
	return true;
}

void UFlecsGameInstanceSubsystem::DetachWorld(UWorld& InWorld)
{
	check(IsInGameThread());
	if (ActiveWorld.Get() != &InWorld)
	{
		return;
	}
	OnWorldAboutToDetach.Broadcast(InWorld);
	ActiveWorld.Reset();
	StopRunner();
}

bool UFlecsGameInstanceSubsystem::IsWorldReady(const UWorld& InWorld) const
{
	check(IsInGameThread());
	return !bIsShuttingDown && ActiveWorld.Get() == &InWorld && Runner.IsValid();
}

uint64 UFlecsGameInstanceSubsystem::GetActiveEpoch() const
{
	check(IsInGameThread());
	UWorld* World = ActiveWorld.Get();
	return World && IsWorldReady(*World) ? Runner->GetEpoch() : 0;
}

EFlecsWorkerCommandResult UFlecsGameInstanceSubsystem::SubmitWorkerCommand(
	uint64 ExpectedEpoch, FFlecsWorkerCommand&& Command)
{
	check(IsInGameThread());
	if (!Runner.IsValid() || !ActiveWorld.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Rejected Flecs worker command because no world session is active."));
		return EFlecsWorkerCommandResult::Stopping;
	}
	const EFlecsWorkerCommandResult Result = Runner->Submit(ExpectedEpoch, MoveTemp(Command));
	if (Result != EFlecsWorkerCommandResult::Accepted)
	{
		UE_LOG(LogTemp, Warning, TEXT("Rejected Flecs worker command: result %u, expected epoch %llu, active epoch %llu."),
			static_cast<uint32>(Result), ExpectedEpoch, Runner->GetEpoch());
	}
	return Result;
}

bool UFlecsGameInstanceSubsystem::RegisterPersistentWorkerCommand(FName Name, FFlecsWorkerCommand&& Command)
{
	check(IsInGameThread());
	if (Name.IsNone() || !Command)
	{
		return false;
	}
	PersistentWorkerCommands.Add(Name, Command);
	if (!Runner.IsValid())
	{
		return true;
	}
	const EFlecsWorkerCommandResult Result = Runner->Submit(Runner->GetEpoch(), MoveTemp(Command));
	if (Result != EFlecsWorkerCommandResult::Accepted)
	{
		UE_LOG(LogTemp, Warning, TEXT("Persistent Flecs worker command '%s' was retained for the next session but not queued: result %u."),
			*Name.ToString(), static_cast<uint32>(Result));
	}
	return Result == EFlecsWorkerCommandResult::Accepted;
}

TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> UFlecsGameInstanceSubsystem::GetPublication() const
{
	check(IsInGameThread());
	return Runner.IsValid() ? Runner->GetPublication() : nullptr;
}

void UFlecsGameInstanceSubsystem::SetSimulationPaused(bool bPaused)
{
	check(IsInGameThread());
	if (Runner.IsValid())
	{
		Runner->SetPaused(bPaused);
	}
}

void UFlecsGameInstanceSubsystem::StopRunner()
{
	if (Runner.IsValid())
	{
		Runner->StopAndJoin();
		Runner.Reset();
	}
}

FName UFlecsGameInstanceSubsystem::MakeWorldSystemName(FName InSystemName) const
{
	return FName(*FString::Printf(TEXT("World_%llu_%s"), WorldGeneration, *InSystemName.ToString()));
}
