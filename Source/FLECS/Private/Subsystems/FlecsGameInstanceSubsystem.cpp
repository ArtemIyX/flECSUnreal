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
	if (!Runner->Start())
	{
		Runner.Reset();
		ActiveWorld.Reset();
		return false;
	}
	for (const TPair<FName, FFlecsWorkerCommand>& Entry : PersistentWorkerCommands)
	{
		FFlecsWorkerCommand Command = Entry.Value;
		if (!Runner->Submit(MoveTemp(Command)))
		{
			StopRunner();
			ActiveWorld.Reset();
			return false;
		}
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

bool UFlecsGameInstanceSubsystem::SubmitWorkerCommand(FFlecsWorkerCommand&& Command)
{
	check(IsInGameThread());
	return Runner.IsValid() && Runner->Submit(MoveTemp(Command));
}

bool UFlecsGameInstanceSubsystem::RegisterPersistentWorkerCommand(FName Name, FFlecsWorkerCommand&& Command)
{
	check(IsInGameThread());
	if (Name.IsNone() || !Command)
	{
		return false;
	}
	PersistentWorkerCommands.Add(Name, Command);
	return !Runner.IsValid() || Runner->Submit(MoveTemp(Command));
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
