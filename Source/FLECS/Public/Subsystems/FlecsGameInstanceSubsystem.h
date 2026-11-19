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
	FName MakeWorldSystemName(FName InSystemName) const;

	/** Queue bounded work for the simulation thread. The callback runs on that thread
	 * and must not capture or access any UObject or Unreal connection state. */
	bool SubmitWorkerCommand(FFlecsWorkerCommand&& Command);
	bool RegisterPersistentWorkerCommand(FName Name, FFlecsWorkerCommand&& Command);
	TSharedPtr<const FFlecsSimulationPublication, ESPMode::ThreadSafe> GetPublication() const;
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
};
