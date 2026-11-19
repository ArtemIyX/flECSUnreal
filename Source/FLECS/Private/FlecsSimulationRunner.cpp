#include "FlecsSimulationRunner.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"

FFlecsSimulationRunner::FFlecsSimulationRunner(uint64 InEpoch)
	: Epoch(InEpoch)
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	InitializedEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

FFlecsSimulationRunner::~FFlecsSimulationRunner()
{
	StopAndJoin();
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
	if (InitializedEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(InitializedEvent);
		InitializedEvent = nullptr;
	}
}

bool FFlecsSimulationRunner::Start(TArray<FWorkerCommand>&& InStartupCommands)
{
	if (Thread || !WakeEvent || !InitializedEvent || InStartupCommands.Num() > MaxQueuedCommands)
	{
		return false;
	}
	for (const FWorkerCommand& Command : InStartupCommands)
	{
		if (!Command)
		{
			return false;
		}
	}
	StartupCommands = MoveTemp(InStartupCommands);
	bInitializationSucceeded.store(false, std::memory_order_release);
	bStopping.store(false, std::memory_order_release);
	Thread = FRunnableThread::Create(this, TEXT("FlecsSimulation"), 0, TPri_Normal);
	if (!Thread)
	{
		return false;
	}
	InitializedEvent->Wait();
	if (!bInitializationSucceeded.load(std::memory_order_acquire))
	{
		StopAndJoin();
		return false;
	}
	return true;
}

void FFlecsSimulationRunner::StopAndJoin()
{
	if (!Thread)
	{
		return;
	}
	bStopping.store(true, std::memory_order_release);
	WakeEvent->Trigger();
	Thread->WaitForCompletion();
	delete Thread;
	Thread = nullptr;
}

EFlecsWorkerCommandResult FFlecsSimulationRunner::Submit(uint64 ExpectedEpoch, FWorkerCommand&& Command)
{
	if (!Command)
	{
		return EFlecsWorkerCommandResult::InvalidCommand;
	}
	if (ExpectedEpoch != Epoch)
	{
		return EFlecsWorkerCommandResult::StaleEpoch;
	}
	if (bStopping.load(std::memory_order_acquire))
	{
		return EFlecsWorkerCommandResult::Stopping;
	}
	{
		std::lock_guard<std::mutex> Lock(QueueMutex);
		if (bStopping.load(std::memory_order_relaxed))
		{
			return EFlecsWorkerCommandResult::Stopping;
		}
		if (Commands.size() >= MaxQueuedCommands)
		{
			RejectedCommands.fetch_add(1, std::memory_order_relaxed);
			return EFlecsWorkerCommandResult::QueueFull;
		}
		Commands.push_back({ std::move(Command), FPlatformTime::Seconds() });
	}
	return EFlecsWorkerCommandResult::Accepted;
}

void FFlecsSimulationRunner::SetPaused(bool bInPaused)
{
	bPaused.store(bInPaused, std::memory_order_release);
	WakeEvent->Trigger();
}

TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> FFlecsSimulationRunner::GetPublication() const
{
	FScopeLock Lock(&PublicationMutex);
	return Publication;
}

uint32 FFlecsSimulationRunner::Run()
{
	// The Flecs world is intentionally created and destroyed on this thread.
	OwnerThreadId = FPlatformTLS::GetCurrentThreadId();
	flecs::world World;
	for (FWorkerCommand& Command : StartupCommands)
	{
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		Command(World);
	}
	StartupCommands.Empty();
	bInitializationSucceeded.store(true, std::memory_order_release);
	InitializedEvent->Trigger();
	double Deadline = FPlatformTime::Seconds();
	bool bUsedImmediateCatchUp = false;
	while (!bStopping.load(std::memory_order_acquire))
	{
		if (bPaused.load(std::memory_order_acquire))
		{
			CommandsAppliedLastStep = 0;
			bUsedImmediateCatchUp = false;
			Publish(0.0);
			WakeEvent->Wait();
			if (bStopping.load(std::memory_order_acquire))
			{
				break;
			}
			if (bPaused.load(std::memory_order_acquire))
			{
				continue;
			}
			// Paused time does not accumulate; resume schedules the next boundary now.
			Deadline = FPlatformTime::Seconds();
			continue;
		}

		const double Now = FPlatformTime::Seconds();
		if (Now < Deadline)
		{
			WakeEvent->Wait(static_cast<uint32>((Deadline - Now) * 1000.0));
			continue;
		}
		if (bPaused.load(std::memory_order_acquire))
		{
			continue;
		}

		const double StartTime = FPlatformTime::Seconds();
		LastDeadlineLatenessSeconds = FMath::Max(0.0, StartTime - Deadline);
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		// Commands are applied only at a fixed-step boundary. The per-step cap
		// ensures sustained ingress cannot postpone simulation progress indefinitely.
		CommandsAppliedLastStep = 0;
		while (CommandsAppliedLastStep < MaxCommandsPerStep)
		{
			FQueuedCommand Command;
			{
				std::lock_guard<std::mutex> Lock(QueueMutex);
				if (Commands.empty())
				{
					break;
				}
				Command = std::move(Commands.front());
				Commands.pop_front();
			}
			if (bStopping.load(std::memory_order_acquire))
			{
				break;
			}
			Command.Callback(World);
			++CommandsAppliedLastStep;
		}
		if (bStopping.load(std::memory_order_acquire))
		{
			break;
		}
		World.progress(static_cast<ecs_ftime_t>(FixedStepSeconds));
		const double EndTime = FPlatformTime::Seconds();
		++Tick;
		++Steps;
		const double StepSeconds = EndTime - StartTime;
		MaxStepSeconds = FMath::Max(MaxStepSeconds, StepSeconds);
		Deadline += FixedStepSeconds;
		// Allow at most one immediate catch-up step. Further lateness skips stale
		// wall-clock deadlines rather than entering an unbounded catch-up loop.
		const double Lateness = EndTime - Deadline;
		if (Lateness > 0.0)
		{
			if (!bUsedImmediateCatchUp && Lateness <= FixedStepSeconds)
			{
				bUsedImmediateCatchUp = true;
			}
			else
			{
				const uint64 SkippedDeadlines = static_cast<uint64>(Lateness / FixedStepSeconds) + 1;
				MissedDeadlines += SkippedDeadlines;
				Deadline += SkippedDeadlines * FixedStepSeconds;
				bUsedImmediateCatchUp = false;
			}
		}
		else
		{
			bUsedImmediateCatchUp = false;
		}
		Publish(StepSeconds);
	}
	return 0;
}

void FFlecsSimulationRunner::Stop()
{
	bStopping.store(true, std::memory_order_release);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}
}

void FFlecsSimulationRunner::Exit()
{
	// Run owns and has already destroyed the world before thread completion.
}

void FFlecsSimulationRunner::Publish(double StepSeconds)
{
	auto NewPublication = MakeShared<FFlecsSimulationPublication, ESPMode::ThreadSafe>();
	NewPublication->Epoch = Epoch;
	NewPublication->Tick = Tick;
	NewPublication->Steps = Steps;
	NewPublication->MissedDeadlines = MissedDeadlines;
	NewPublication->RejectedCommands = RejectedCommands.load(std::memory_order_relaxed);
	NewPublication->CommandsAppliedLastStep = CommandsAppliedLastStep;
	NewPublication->LastStepSeconds = StepSeconds;
	NewPublication->MaxStepSeconds = MaxStepSeconds;
	NewPublication->LastDeadlineLatenessSeconds = LastDeadlineLatenessSeconds;
	{
		std::lock_guard<std::mutex> QueueLock(QueueMutex);
		NewPublication->QueueDepth = static_cast<int32>(Commands.size());
		if (!Commands.empty())
		{
			NewPublication->OldestCommandAgeSeconds = FMath::Max(0.0, FPlatformTime::Seconds() - Commands.front().EnqueuedAt);
		}
	}
	FScopeLock Lock(&PublicationMutex);
	Publication = MoveTemp(NewPublication);
}
