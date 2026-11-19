#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Misc/Build.h"

#include "FlecsSimulationTypes.h"
#include "Containers/StringConv.h"
#include <string>
#include "FlecsGameInstanceSubsystem.generated.h"

class UWorld;
class FFlecsSimulationRunner;

DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsWorldLifecycleEvent, UWorld&);

UCLASS()
class FLECS_API UFlecsGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool AttachWorld(UWorld& InWorld);
	void DetachWorld(UWorld& InWorld);
	bool IsWorldReady(const UWorld& InWorld) const;
	uint64 GetWorldSerial() const { return WorldSerial; }
	uint64 GetWorldGeneration() const { return WorldGeneration; }
	uint64 GetActiveEpoch() const;
	FName MakeWorldSystemName(FName InSystemName) const;

	/** Queue bounded work for the simulation thread. The callback runs on that thread
	 * and must not capture or access any UObject or Unreal connection state. */
	EFlecsWorkerCommandResult SubmitWorkerCommand(uint64 ExpectedEpoch, FFlecsWorkerCommand&& Command);
	bool RegisterPersistentWorkerCommand(FName Name, FFlecsWorkerCommand&& Command);
	/** Register or replace a bounded DTO extractor. ExpectedEpoch must be zero
	 * before attachment, or the active local attachment epoch for a live session.
	 * Active changes take effect at the next fixed-step boundary. Queue saturation
	 * leaves the prior registration unchanged. The callback runs on the worker and
	 * must not access UObjects or connection state. */
	EFlecsWorkerCommandResult RegisterPersistentDataPublisher(uint64 ExpectedEpoch, FName Name,
		FFlecsWorkerDataPublisher&& Publisher);
	/** Remove a publisher at the next step boundary. Zero is valid only before attach;
	 * active callers must supply their expected local attachment epoch. */
	EFlecsWorkerCommandResult UnregisterPersistentDataPublisher(uint64 ExpectedEpoch, FName Name);
	/** Install a paired worker-only lifecycle callback. Startup runs before the first
	 * progress call; shutdown runs exactly once on the worker before world destruction. */
	EFlecsWorkerCommandResult RegisterPersistentWorkerLifecycle(uint64 ExpectedEpoch, FName Name,
		FFlecsWorkerStartupCallback Startup, FFlecsWorkerShutdownCallback Shutdown);
	EFlecsWorkerCommandResult UnregisterPersistentWorkerLifecycle(uint64 ExpectedEpoch, FName Name);
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> GetPublication() const;
	TSharedPtr<const FFlecsSimulationDataPublication, ESPMode::ThreadSafe> GetDataPublication() const;
	void SetSimulationPaused(bool bPaused);

	FFlecsWorldLifecycleEvent OnWorldAboutToDetach;
	FFlecsWorldLifecycleEvent OnWorldAttached;

	template <typename... Components, typename FuncType>
	bool RegisterPersistentOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		if (SystemName.IsNone())
		{
			return false;
		}
		FTCHARToUTF8 ConvertedName(*SystemName.ToString());
		const std::string WorkerName(ConvertedName.Get());
		return RegisterPersistentWorkerCommand(SystemName, [WorkerName, Callback = Forward<FuncType>(Func)](flecs::world& World) mutable
		{
			flecs::entity Existing = World.lookup(WorkerName.c_str());
			if (Existing.is_alive())
			{
				Existing.destruct();
			}
			World.system<Components...>(WorkerName.c_str())
				.kind(flecs::OnUpdate)
				.each(MoveTemp(Callback));
		});
	}

private:
	void StopRunner();

	TSharedPtr<FFlecsSimulationRunner, ESPMode::ThreadSafe> Runner;
	TWeakObjectPtr<UWorld> ActiveWorld;
	uint64 WorldGeneration = 0;
	uint64 WorldSerial = 0;
	bool bIsShuttingDown = false;
	TMap<FName, FFlecsWorkerCommand> PersistentWorkerCommands;
	TMap<FName, FFlecsWorkerDataPublisher> PersistentDataPublishers;
	TMap<FName, FFlecsNamedWorkerLifecycle> PersistentWorkerLifecycles;
};
