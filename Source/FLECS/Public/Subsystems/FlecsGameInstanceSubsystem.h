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
	/** Register a bounded DTO extractor before world attachment. It runs on the
	 * simulation worker after each fixed step; no UObject access is allowed. Each
	 * publication is capped by FlecsMaxDataPublication* in FlecsSimulationTypes.h;
	 * at most FlecsMaxDataPublishers extractors can be registered. */
	bool RegisterPersistentDataPublisher(FName Name, FFlecsWorkerDataPublisher&& Publisher);
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
};
