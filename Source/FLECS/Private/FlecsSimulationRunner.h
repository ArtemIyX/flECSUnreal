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
		FWorkerCommand Callback;
		double EnqueuedAt = 0.0;
	};
	static constexpr uint64 MaxQueuedCommands = 1024;
	static constexpr uint64 MaxCommandsPerStep = 64;
	static constexpr double FixedStepSeconds = 1.0 / 60.0;

	explicit FFlecsSimulationRunner(uint64 InEpoch);
	~FFlecsSimulationRunner();
	FFlecsSimulationRunner(const FFlecsSimulationRunner&) = delete;
	FFlecsSimulationRunner& operator=(const FFlecsSimulationRunner&) = delete;

	bool Start(TArray<FWorkerCommand>&& StartupCommands);
	bool Start() { return Start(TArray<FWorkerCommand>()); }
	void StopAndJoin();
	EFlecsWorkerCommandResult Submit(uint64 ExpectedEpoch, FWorkerCommand&& Command);
	bool Submit(FWorkerCommand&& Command)
	{
		return Submit(Epoch, MoveTemp(Command)) == EFlecsWorkerCommandResult::Accepted;
	}
	void SetPaused(bool bInPaused);
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> GetPublication() const;
	uint64 GetEpoch() const { return Epoch; }

private:
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;
	void Publish(double StepSeconds);

	const uint64 Epoch;
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	FEvent* InitializedEvent = nullptr;
	TArray<FWorkerCommand> StartupCommands;
	std::atomic<bool> bInitializationSucceeded{ false };
	std::mutex QueueMutex;
	std::deque<FQueuedCommand> Commands;
	std::atomic<bool> bStopping{ false };
	std::atomic<bool> bPaused{ false };
	mutable FCriticalSection PublicationMutex;
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> Publication;
	uint64 Tick = 0;
	uint64 Steps = 0;
	uint64 MissedDeadlines = 0;
	uint64 CommandsAppliedLastStep = 0;
	std::atomic<uint64> RejectedCommands{ 0 };
	double MaxStepSeconds = 0.0;
	double LastDeadlineLatenessSeconds = 0.0;
	uint32 OwnerThreadId = 0;
};
