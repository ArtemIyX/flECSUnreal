#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "FlecsSimulationTypes.h"

#include <deque>
#include <atomic>
#include <functional>
#include <mutex>

/** Owns every access to its Flecs world, including construction and destruction. */
class FFlecsSimulationRunner final : private FRunnable
{
public:
	using FWorkerCommand = FFlecsWorkerCommand;
	struct FQueuedCommand
	{
		enum class EKind : uint8 { Command, AddPublisher, RemovePublisher, AddLifecycle, RemoveLifecycle } Kind = EKind::Command;
		FWorkerCommand Callback;
		FName Name;
		FFlecsWorkerDataPublisher Publisher;
		FFlecsWorkerStartupCallback Startup;
		FFlecsWorkerShutdownCallback Shutdown;
		double EnqueuedAt = 0.0;
	};
	static constexpr uint64 MaxQueuedCommands = 1024;
	static constexpr uint64 MaxCommandsPerStep = 64;
	static constexpr uint32 MaxPublicationExtractors = FlecsMaxDataPublishers;
	static constexpr double FixedStepSeconds = 1.0 / 60.0;

	explicit FFlecsSimulationRunner(uint64 InEpoch);
	~FFlecsSimulationRunner();
	FFlecsSimulationRunner(const FFlecsSimulationRunner&) = delete;
	FFlecsSimulationRunner& operator=(const FFlecsSimulationRunner&) = delete;

	bool Start(TArray<FWorkerCommand>&& StartupCommands, TArray<FFlecsNamedWorkerDataPublisher>&& StartupPublishers,
		TArray<FFlecsNamedWorkerLifecycle>&& StartupLifecycles = {});
	bool Start(TArray<FWorkerCommand>&& StartupCommands, TArray<FFlecsWorkerDataPublisher>&& StartupPublishers)
	{
		TArray<FFlecsNamedWorkerDataPublisher> NamedPublishers;
		NamedPublishers.Reserve(StartupPublishers.Num());
		for (int32 Index = 0; Index < StartupPublishers.Num(); ++Index)
		{
			NamedPublishers.Add({ FName(*FString::Printf(TEXT("LegacyPublisher_%d"), Index)), MoveTemp(StartupPublishers[Index]) });
		}
		return Start(MoveTemp(StartupCommands), MoveTemp(NamedPublishers), TArray<FFlecsNamedWorkerLifecycle>());
	}
	bool Start() { return Start(TArray<FWorkerCommand>(), TArray<FFlecsNamedWorkerDataPublisher>(), TArray<FFlecsNamedWorkerLifecycle>()); }
	void StopAndJoin();
	EFlecsWorkerCommandResult Submit(uint64 ExpectedEpoch, FWorkerCommand&& Command);
	EFlecsWorkerCommandResult RegisterPublisher(uint64 ExpectedEpoch, FName Name, FFlecsWorkerDataPublisher&& Publisher);
	EFlecsWorkerCommandResult UnregisterPublisher(uint64 ExpectedEpoch, FName Name);
	EFlecsWorkerCommandResult RegisterLifecycle(uint64 ExpectedEpoch, FName Name,
		FFlecsWorkerStartupCallback&& Startup, FFlecsWorkerShutdownCallback&& Shutdown);
	EFlecsWorkerCommandResult UnregisterLifecycle(uint64 ExpectedEpoch, FName Name);
	bool Submit(FWorkerCommand&& Command)
	{
		return Submit(Epoch, MoveTemp(Command)) == EFlecsWorkerCommandResult::Accepted;
	}
	void SetPaused(bool bInPaused);
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> GetPublication() const;
	TSharedPtr<const FFlecsSimulationDataPublication, ESPMode::ThreadSafe> GetDataPublication() const;
	uint64 GetEpoch() const { return Epoch; }

private:
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	void Publish(double StepSeconds);
	void PublishData(flecs::world& World);
	uint64 ProcessQueuedWork(flecs::world& World, uint64 MaxItems, bool bShutdownDrain);
	void ApplyQueuedWork(flecs::world& World, FQueuedCommand&& Work, bool bShutdownDrain);
	void ShutdownLifecycles(flecs::world& World);

	const uint64 Epoch;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	FEvent* InitializedEvent = nullptr;
	TArray<FWorkerCommand> StartupCommands;
	TArray<FFlecsNamedWorkerDataPublisher> StartupPublishers;
	TArray<FFlecsNamedWorkerLifecycle> StartupLifecycles;
	TMap<FName, FFlecsWorkerDataPublisher> DataPublishers;
	TMap<FName, FFlecsWorkerShutdownCallback> ActiveLifecycles;
	std::atomic<bool> bInitializationSucceeded{ false };
	std::mutex QueueMutex;
	std::deque<FQueuedCommand> Commands;
	std::atomic<bool> bStopping{ false };
	std::atomic<bool> bPaused{ false };
	mutable FCriticalSection PublicationMutex;
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> Publication;
	TSharedPtr<const FFlecsSimulationDataPublication, ESPMode::ThreadSafe> DataPublication;
	uint64 Tick = 0;
	uint64 Steps = 0;
	uint64 MissedDeadlines = 0;
	uint64 CommandsAppliedLastStep = 0;
	uint64 PublishedDataItems = 0;
	uint64 PublishedDataBytes = 0;
	uint64 RejectedDataItems = 0;
	std::atomic<uint64> RejectedCommands{ 0 };
	double MaxStepSeconds = 0.0;
	double LastDeadlineLatenessSeconds = 0.0;
	uint32 OwnerThreadId = 0;
};
