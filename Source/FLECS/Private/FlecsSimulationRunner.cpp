#include "FlecsSimulationRunner.h"
#include "Diagnostics/FlecsStats.h"
#include "HAL/IConsoleManager.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformTLS.h"

namespace
{
TAutoConsoleVariable<int32> CVarFlecsSystemTime(TEXT("flecs.Diagnostics.SystemTime"), 0,
	TEXT("Measure time spent inside Flecs systems (0 off, 1 on)."), ECVF_Default);
TAutoConsoleVariable<int32> CVarFlecsWorldMemory(TEXT("flecs.Diagnostics.WorldMemory"), 0,
	TEXT("Sample Flecs-reported world memory at most once per second (0 off, 1 on)."), ECVF_Default);
}

FFlecsSimulationRunner::FFlecsSimulationRunner(uint64 InEpoch, double InSimulationRateHz)
	: Epoch(InEpoch)
	, SimulationStepSeconds(
		  1.0 / (FMath::IsFinite(InSimulationRateHz) ? FMath::Clamp(InSimulationRateHz, 1.0, 240.0) : 60.0))
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

bool FFlecsSimulationRunner::Start(TArray<FWorkerCommand>&& InStartupCommands,
	TArray<FFlecsNamedWorkerDataPublisher>&& InStartupPublishers,
	TArray<FFlecsNamedWorkerLifecycle>&& InStartupLifecycles)
{
	if (Thread || !WakeEvent || !InitializedEvent || InStartupCommands.Num() > MaxQueuedCommands
		|| InStartupPublishers.Num() > static_cast<int32>(MaxPublicationExtractors)
		|| InStartupLifecycles.Num() > static_cast<int32>(MaxPublicationExtractors))
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
	TSet<FName> PublisherNames;
	for (const FFlecsNamedWorkerDataPublisher& Publisher : InStartupPublishers)
	{
		if (Publisher.Name.IsNone() || !Publisher.Callback || PublisherNames.Contains(Publisher.Name))
		{
			return false;
		}
		PublisherNames.Add(Publisher.Name);
	}
	TSet<FName> LifecycleNames;
	for (const FFlecsNamedWorkerLifecycle& Lifecycle : InStartupLifecycles)
	{
		if (Lifecycle.Name.IsNone() || !Lifecycle.Startup || !Lifecycle.Shutdown
			|| LifecycleNames.Contains(Lifecycle.Name))
		{
			return false;
		}
		LifecycleNames.Add(Lifecycle.Name);
	}
	StartupCommands = MoveTemp(InStartupCommands);
	StartupPublishers = MoveTemp(InStartupPublishers);
	StartupLifecycles = MoveTemp(InStartupLifecycles);
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
		FQueuedCommand Work;
		Work.Kind = FQueuedCommand::EKind::Command;
		Work.Callback = MoveTemp(Command);
		Work.EnqueuedAt = FPlatformTime::Seconds();
		Commands.push_back(MoveTemp(Work));
	}
	return EFlecsWorkerCommandResult::Accepted;
}

EFlecsWorkerCommandResult FFlecsSimulationRunner::RegisterPublisher(
	uint64 ExpectedEpoch, FName Name, FFlecsWorkerDataPublisher&& Publisher)
{
	if (ExpectedEpoch != Epoch)
		return EFlecsWorkerCommandResult::StaleEpoch;
	if (Name.IsNone() || !Publisher)
		return EFlecsWorkerCommandResult::InvalidCommand;
	if (bStopping.load(std::memory_order_acquire))
		return EFlecsWorkerCommandResult::Stopping;
	std::lock_guard<std::mutex> Lock(QueueMutex);
	if (bStopping.load(std::memory_order_relaxed))
		return EFlecsWorkerCommandResult::Stopping;
	if (Commands.size() >= MaxQueuedCommands)
	{
		RejectedCommands.fetch_add(1, std::memory_order_relaxed);
		return EFlecsWorkerCommandResult::QueueFull;
	}
	FQueuedCommand Work;
	Work.Kind = FQueuedCommand::EKind::AddPublisher;
	Work.Name = Name;
	Work.Publisher = MoveTemp(Publisher);
	Work.EnqueuedAt = FPlatformTime::Seconds();
	Commands.push_back(MoveTemp(Work));
	return EFlecsWorkerCommandResult::Accepted;
}

EFlecsWorkerCommandResult FFlecsSimulationRunner::UnregisterPublisher(uint64 ExpectedEpoch, FName Name)
{
	if (ExpectedEpoch != Epoch)
		return EFlecsWorkerCommandResult::StaleEpoch;
	if (Name.IsNone())
		return EFlecsWorkerCommandResult::InvalidCommand;
	if (bStopping.load(std::memory_order_acquire))
		return EFlecsWorkerCommandResult::Stopping;
	std::lock_guard<std::mutex> Lock(QueueMutex);
	if (bStopping.load(std::memory_order_relaxed))
		return EFlecsWorkerCommandResult::Stopping;
	if (Commands.size() >= MaxQueuedCommands)
	{
		RejectedCommands.fetch_add(1, std::memory_order_relaxed);
		return EFlecsWorkerCommandResult::QueueFull;
	}
	FQueuedCommand Work;
	Work.Kind = FQueuedCommand::EKind::RemovePublisher;
	Work.Name = Name;
	Work.EnqueuedAt = FPlatformTime::Seconds();
	Commands.push_back(MoveTemp(Work));
	return EFlecsWorkerCommandResult::Accepted;
}

EFlecsWorkerCommandResult FFlecsSimulationRunner::RegisterLifecycle(
	uint64 ExpectedEpoch, FName Name, FFlecsWorkerStartupCallback&& Startup, FFlecsWorkerShutdownCallback&& Shutdown)
{
	if (ExpectedEpoch != Epoch)
		return EFlecsWorkerCommandResult::StaleEpoch;
	if (Name.IsNone() || !Startup || !Shutdown)
		return EFlecsWorkerCommandResult::InvalidCommand;
	if (bStopping.load(std::memory_order_acquire))
		return EFlecsWorkerCommandResult::Stopping;
	std::lock_guard<std::mutex> Lock(QueueMutex);
	if (bStopping.load(std::memory_order_relaxed))
		return EFlecsWorkerCommandResult::Stopping;
	if (Commands.size() >= MaxQueuedCommands)
	{
		RejectedCommands.fetch_add(1, std::memory_order_relaxed);
		return EFlecsWorkerCommandResult::QueueFull;
	}
	FQueuedCommand Work;
	Work.Kind = FQueuedCommand::EKind::AddLifecycle;
	Work.Name = Name;
	Work.Startup = MoveTemp(Startup);
	Work.Shutdown = MoveTemp(Shutdown);
	Work.EnqueuedAt = FPlatformTime::Seconds();
	Commands.push_back(MoveTemp(Work));
	return EFlecsWorkerCommandResult::Accepted;
}

EFlecsWorkerCommandResult FFlecsSimulationRunner::UnregisterLifecycle(uint64 ExpectedEpoch, FName Name)
{
	if (ExpectedEpoch != Epoch)
		return EFlecsWorkerCommandResult::StaleEpoch;
	if (Name.IsNone())
		return EFlecsWorkerCommandResult::InvalidCommand;
	if (bStopping.load(std::memory_order_acquire))
		return EFlecsWorkerCommandResult::Stopping;
	std::lock_guard<std::mutex> Lock(QueueMutex);
	if (bStopping.load(std::memory_order_relaxed))
		return EFlecsWorkerCommandResult::Stopping;
	if (Commands.size() >= MaxQueuedCommands)
	{
		RejectedCommands.fetch_add(1, std::memory_order_relaxed);
		return EFlecsWorkerCommandResult::QueueFull;
	}
	FQueuedCommand Work;
	Work.Kind = FQueuedCommand::EKind::RemoveLifecycle;
	Work.Name = Name;
	Work.EnqueuedAt = FPlatformTime::Seconds();
	Commands.push_back(MoveTemp(Work));
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

TSharedPtr<const FFlecsSimulationDataPublication, ESPMode::ThreadSafe>
FFlecsSimulationRunner::GetDataPublication() const
{
	FScopeLock Lock(&PublicationMutex);
	return DataPublication;
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
	for (FFlecsNamedWorkerDataPublisher& Publisher : StartupPublishers)
	{
		DataPublishers.Add(Publisher.Name, MoveTemp(Publisher.Callback));
	}
	StartupPublishers.Empty();
	for (FFlecsNamedWorkerLifecycle& Lifecycle : StartupLifecycles)
	{
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		Lifecycle.Startup(World, Epoch);
		ActiveLifecycles.Add(Lifecycle.Name, MoveTemp(Lifecycle.Shutdown));
	}
	StartupLifecycles.Empty();
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

		SCOPE_CYCLE_COUNTER(STAT_FlecsWorkerStep);
		const double StartTime = FPlatformTime::Seconds();
		LastDeadlineLatenessSeconds = FMath::Max(0.0, StartTime - Deadline);
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		// Commands are applied only at a fixed-step boundary. The per-step cap
		// ensures sustained ingress cannot postpone simulation progress indefinitely.
		CommandsAppliedLastStep = ProcessQueuedWork(World, MaxCommandsPerStep, false);
		if (bStopping.load(std::memory_order_acquire))
		{
			break;
		}
		INC_DWORD_STAT_BY(STAT_FlecsCommands, CommandsAppliedLastStep);
		const bool bMeasureSystems = CVarFlecsSystemTime.GetValueOnAnyThread() != 0;
		if (bMeasureSystems != bSystemTimeAvailable)
		{
			ecs_measure_system_time(World.c_ptr(), bMeasureSystems);
			bSystemTimeAvailable = bMeasureSystems;
		}
		const double PreviousSystemTime = bMeasureSystems ? ecs_get_world_info(World.c_ptr())->system_time_total : 0.0;
		{
			SCOPE_CYCLE_COUNTER(STAT_FlecsWorldProgress);
			World.progress(static_cast<ecs_ftime_t>(SimulationStepSeconds));
		}
		LastSystemSeconds =
			bMeasureSystems ? FMath::Max(0.0, static_cast<double>(ecs_get_world_info(World.c_ptr())->system_time_total)
												  - PreviousSystemTime)
							: 0.0;
		if (bMeasureSystems)
		{
			INC_FLOAT_STAT_BY(STAT_FlecsSystemMilliseconds, LastSystemSeconds * 1000.0);
		}
		if (CVarFlecsWorldMemory.GetValueOnAnyThread() != 0)
		{
			if (StartTime >= NextMemorySampleSeconds)
			{
				const ecs_size_t Measured = ecs_memory_get(World.c_ptr());
				const uint64 Sample = Measured >= 0 ? static_cast<uint64>(Measured) : 0;
				if (Sample >= WorldMemoryBytes)
				{
					INC_MEMORY_STAT_BY(STAT_FlecsWorldMemory, Sample - WorldMemoryBytes);
				}
				else
				{
					DEC_MEMORY_STAT_BY(STAT_FlecsWorldMemory, WorldMemoryBytes - Sample);
				}
				WorldMemoryBytes = Sample;
				bWorldMemoryAvailable = Measured >= 0;
				NextMemorySampleSeconds = StartTime + 1.0;
			}
		}
		else if (bWorldMemoryAvailable)
		{
			DEC_MEMORY_STAT_BY(STAT_FlecsWorldMemory, WorldMemoryBytes);
			WorldMemoryBytes = 0;
			bWorldMemoryAvailable = false;
			NextMemorySampleSeconds = 0.0;
		}
		++Tick;
		++Steps;
		PublishData(World);
		const double EndTime = FPlatformTime::Seconds();
		const double StepSeconds = EndTime - StartTime;
		MaxStepSeconds = FMath::Max(MaxStepSeconds, StepSeconds);
		Deadline += SimulationStepSeconds;
		// Allow at most one immediate catch-up step. Further lateness skips stale
		// wall-clock deadlines rather than entering an unbounded catch-up loop.
		const double Lateness = EndTime - Deadline;
		if (Lateness > 0.0)
		{
			if (!bUsedImmediateCatchUp && Lateness <= SimulationStepSeconds)
			{
				bUsedImmediateCatchUp = true;
			}
			else
			{
				const uint64 SkippedDeadlines = static_cast<uint64>(Lateness / SimulationStepSeconds) + 1;
				MissedDeadlines += SkippedDeadlines;
				INC_DWORD_STAT_BY(
					STAT_FlecsMissedDeadlines, static_cast<uint32>(FMath::Min<uint64>(SkippedDeadlines, MAX_uint32)));
				Deadline += SkippedDeadlines * SimulationStepSeconds;
				bUsedImmediateCatchUp = false;
			}
		}
		else
		{
			bUsedImmediateCatchUp = false;
		}
		Publish(StepSeconds);
	}
	// Stop admission happens before the wake. Resolve already accepted lifecycle
	// controls (and discard gameplay commands) before paired shutdown callbacks.
	ProcessQueuedWork(World, MaxQueuedCommands, true);
	ShutdownLifecycles(World);
	if (bWorldMemoryAvailable)
	{
		DEC_MEMORY_STAT_BY(STAT_FlecsWorldMemory, WorldMemoryBytes);
	}
	DEC_MEMORY_STAT_BY(STAT_FlecsPublicationMemory, PublicationAllocatedBytes);
	return 0;
}

uint64 FFlecsSimulationRunner::ProcessQueuedWork(flecs::world& World, uint64 MaxItems, bool bShutdownDrain)
{
	uint64 Processed = 0;
	while (Processed < MaxItems)
	{
		FQueuedCommand Work;
		{
			std::lock_guard<std::mutex> Lock(QueueMutex);
			if (Commands.empty())
				break;
			Work = MoveTemp(Commands.front());
			Commands.pop_front();
		}
		ApplyQueuedWork(World, MoveTemp(Work), bShutdownDrain);
		++Processed;
	}
	return Processed;
}

void FFlecsSimulationRunner::ApplyQueuedWork(flecs::world& World, FQueuedCommand&& Work, bool bShutdownDrain)
{
	check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
	switch (Work.Kind)
	{
		case FQueuedCommand::EKind::Command:
			if (!bShutdownDrain && Work.Callback)
				Work.Callback(World);
			break;
		case FQueuedCommand::EKind::AddPublisher:
			if (Work.Publisher
				&& (DataPublishers.Contains(Work.Name)
					|| DataPublishers.Num() < static_cast<int32>(MaxPublicationExtractors)))
			{
				DataPublishers.Add(Work.Name, MoveTemp(Work.Publisher));
			}
			else
			{
				RejectedCommands.fetch_add(1, std::memory_order_relaxed);
			}
			break;
		case FQueuedCommand::EKind::RemovePublisher:
			DataPublishers.Remove(Work.Name);
			break;
		case FQueuedCommand::EKind::AddLifecycle:
			if (!Work.Startup || !Work.Shutdown)
				break;
			if (FFlecsWorkerShutdownCallback* Existing = ActiveLifecycles.Find(Work.Name))
			{
				(*Existing)(World, Epoch);
				ActiveLifecycles.Remove(Work.Name);
			}
			if (ActiveLifecycles.Num() >= static_cast<int32>(MaxPublicationExtractors))
			{
				RejectedCommands.fetch_add(1, std::memory_order_relaxed);
				break;
			}
			Work.Startup(World, Epoch);
			ActiveLifecycles.Add(Work.Name, MoveTemp(Work.Shutdown));
			break;
		case FQueuedCommand::EKind::RemoveLifecycle:
			if (FFlecsWorkerShutdownCallback* Existing = ActiveLifecycles.Find(Work.Name))
			{
				(*Existing)(World, Epoch);
				ActiveLifecycles.Remove(Work.Name);
			}
			break;
	}
}

void FFlecsSimulationRunner::ShutdownLifecycles(flecs::world& World)
{
	check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
	TArray<FName> Names;
	ActiveLifecycles.GetKeys(Names);
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName Name : Names)
	{
		if (FFlecsWorkerShutdownCallback* Shutdown = ActiveLifecycles.Find(Name))
		{
			(*Shutdown)(World, Epoch);
		}
	}
	ActiveLifecycles.Empty();
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
	NewPublication->PublishedDataItems = PublishedDataItems;
	NewPublication->PublishedDataBytes = PublishedDataBytes;
	NewPublication->PublicationAllocatedBytes = PublicationAllocatedBytes;
	NewPublication->WorldMemoryBytes = WorldMemoryBytes;
	NewPublication->bWorldMemoryAvailable = bWorldMemoryAvailable;
	NewPublication->RejectedDataItems = RejectedDataItems;
	NewPublication->LastSystemSeconds = LastSystemSeconds;
	NewPublication->bSystemTimeAvailable = bSystemTimeAvailable;
	NewPublication->LastStepSeconds = StepSeconds;
	NewPublication->MaxStepSeconds = MaxStepSeconds;
	NewPublication->LastDeadlineLatenessSeconds = LastDeadlineLatenessSeconds;
	{
		std::lock_guard<std::mutex> QueueLock(QueueMutex);
		NewPublication->QueueDepth = static_cast<int32>(Commands.size());
		INC_DWORD_STAT_BY(STAT_FlecsQueueDepth, NewPublication->QueueDepth);
		if (!Commands.empty())
		{
			NewPublication->OldestCommandAgeSeconds =
				FMath::Max(0.0, FPlatformTime::Seconds() - Commands.front().EnqueuedAt);
		}
	}
	FScopeLock Lock(&PublicationMutex);
	Publication = MoveTemp(NewPublication);
}

void FFlecsSimulationRunner::PublishData(flecs::world& World)
{
	SCOPE_CYCLE_COUNTER(STAT_FlecsPublication);
	FFlecsSimulationDataBuilder Builder;
	for (TPair<FName, FFlecsWorkerDataPublisher>& Entry : DataPublishers)
	{
		check(FPlatformTLS::GetCurrentThreadId() == OwnerThreadId);
		Entry.Value(World, Builder);
	}

	PublishedDataItems = Builder.GetItems().Num();
	PublishedDataBytes = Builder.GetTotalBytes();
	RejectedDataItems = Builder.GetRejectedItems();
	auto NewDataPublication = MakeShared<FFlecsSimulationDataPublication, ESPMode::ThreadSafe>();
	NewDataPublication->LocalAttachmentEpoch = Epoch;
	NewDataPublication->SimulationTick = Tick;
	NewDataPublication->TimestampSeconds = FPlatformTime::Seconds();
	NewDataPublication->TotalBytes = Builder.GetTotalBytes();
	NewDataPublication->RejectedItems = Builder.GetRejectedItems();
	NewDataPublication->Items = Builder.TakeItems();
	uint64 NewAllocatedBytes = sizeof(FFlecsSimulationDataPublication) + NewDataPublication->Items.GetAllocatedSize();
	for (const FFlecsSimulationDataItem& Item : NewDataPublication->Items)
		NewAllocatedBytes += Item.Payload.GetAllocatedSize();
	if (NewAllocatedBytes >= PublicationAllocatedBytes)
	{
		INC_MEMORY_STAT_BY(STAT_FlecsPublicationMemory, NewAllocatedBytes - PublicationAllocatedBytes);
	}
	else
	{
		DEC_MEMORY_STAT_BY(STAT_FlecsPublicationMemory, PublicationAllocatedBytes - NewAllocatedBytes);
	}
	PublicationAllocatedBytes = NewAllocatedBytes;
	INC_DWORD_STAT_BY(STAT_FlecsPublicationItems, PublishedDataItems);
	FScopeLock Lock(&PublicationMutex);
	DataPublication = MoveTemp(NewDataPublication);
}
