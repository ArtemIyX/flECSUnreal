#include "FlecsSimulationRunner.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"

FFlecsSimulationRunner::FFlecsSimulationRunner(uint64 InEpoch)
	: Epoch(InEpoch)
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
}

FFlecsSimulationRunner::~FFlecsSimulationRunner()
{
	StopAndJoin();
	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

bool FFlecsSimulationRunner::Start()
{
	if (Thread || !WakeEvent)
	{
		return false;
	}
	bStopping.store(false, std::memory_order_release);
	Thread = FRunnableThread::Create(this, TEXT("FlecsSimulation"), 0, TPri_Normal);
	return Thread != nullptr;
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

bool FFlecsSimulationRunner::Submit(FWorkerCommand&& Command)
{
	if (!Command || bStopping.load(std::memory_order_acquire))
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> Lock(QueueMutex);
		if (Commands.size() >= MaxQueuedCommands || bStopping.load(std::memory_order_relaxed))
		{
			RejectedCommands.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		Commands.push_back({ std::move(Command), FPlatformTime::Seconds() });
	}
	WakeEvent->Trigger();
	return true;
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
	double Deadline = FPlatformTime::Seconds();
	while (!bStopping.load(std::memory_order_acquire))
	{
		std::deque<FQueuedCommand> Pending;
		{
			std::lock_guard<std::mutex> Lock(QueueMutex);
			Pending.swap(Commands);
		}
		for (FQueuedCommand& Command : Pending)
		{
			check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
			if (bStopping.load(std::memory_order_acquire))
			{
				break;
			}
			Command.Callback(World);
		}

		if (bStopping.load(std::memory_order_acquire))
		{
			break;
		}
		if (bPaused.load(std::memory_order_acquire))
		{
			Publish(0.0);
			WakeEvent->Wait();
			Deadline = FPlatformTime::Seconds();
			continue;
		}

		const double Now = FPlatformTime::Seconds();
		if (Now < Deadline)
		{
			WakeEvent->Wait(static_cast<uint32>((Deadline - Now) * 1000.0));
			continue;
		}

		const double StartTime = FPlatformTime::Seconds();
		LastDeadlineLatenessSeconds = FMath::Max(0.0, StartTime - Deadline);
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		World.progress(static_cast<ecs_ftime_t>(FixedStepSeconds));
		const double EndTime = FPlatformTime::Seconds();
		++Tick;
		++Steps;
		const double StepSeconds = EndTime - StartTime;
		MaxStepSeconds = FMath::Max(MaxStepSeconds, StepSeconds);
		Deadline += FixedStepSeconds;
		// Cap catch-up to one immediate step. Skip stale wall-clock deadlines rather
		// than changing the fixed delta or entering an unbounded catch-up loop.
		if (EndTime - Deadline > FixedStepSeconds)
		{
			++MissedDeadlines;
			Deadline = EndTime + FixedStepSeconds;
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
