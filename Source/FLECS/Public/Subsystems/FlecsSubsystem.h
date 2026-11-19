#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/FlecsGameInstanceSubsystem.h"
#include "Misc/Build.h"
#include <string>
#include "FlecsSubsystem.generated.h"

UCLASS(Blueprintable, BlueprintType)
class FLECS_API UFlecsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	/** Register on the worker. Callbacks execute on the worker and may not capture
	 * or access UObjects, the game thread, or Unreal connection state. */
	template <typename... Components, typename FuncType>
	bool RegisterOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		UFlecsGameInstanceSubsystem* Owner = GetOwnerSubsystem();
		UWorld* WorldContext = GetWorld();
		if (!Owner || !WorldContext || !Owner->IsWorldReady(*WorldContext) || SystemName.IsNone())
		{
			return false;
		}
		const uint64 ExpectedEpoch = Owner->GetActiveEpoch();
		if (ExpectedEpoch == 0)
		{
			return false;
		}
		const FName ScopedName = Owner->MakeWorldSystemName(SystemName);
		FTCHARToUTF8 ConvertedName(*ScopedName.ToString());
		const std::string WorkerName(ConvertedName.Get());
		return Owner->SubmitWorkerCommand(ExpectedEpoch, [WorkerName, Callback = Forward<FuncType>(Func)](
															 flecs::world& World) mutable {
			flecs::entity Existing = World.lookup(WorkerName.c_str());
			if (Existing.is_alive())
			{
				Existing.destruct();
			}
			World.system<Components...>(WorkerName.c_str()).kind(flecs::OnUpdate).each(MoveTemp(Callback));
		}) == EFlecsWorkerCommandResult::Accepted;
	}

	bool UnregisterSystem(FName SystemName);

private:
	UFlecsGameInstanceSubsystem* GetOwnerSubsystem() const;
	UPROPERTY(Transient)
	TWeakObjectPtr<UFlecsGameInstanceSubsystem> CachedOwnerSubsystem;
};
