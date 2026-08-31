#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/StringConv.h"
#include "Misc/Build.h"

#pragma push_macro("FLECS_API")
#undef FLECS_API

PRAGMA_DISABLE_UNREACHABLE_CODE_WARNINGS
#include "flecs.h"

#pragma pop_macro("FLECS_API")

#include "FlecsGameInstanceSubsystem.generated.h"

class UWorld;

UCLASS()
class FLECS_API UFlecsGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	flecs::world* GetEcsWorld();
	const flecs::world* GetEcsWorld() const;
	flecs::entity GetActiveWorldScope() const;

	bool AttachWorld(UWorld& InWorld);
	void DetachWorld(UWorld& InWorld);
	bool ProgressFromWorld(UWorld& InWorld, float DeltaTime);
	FName MakeWorldSystemName(FName InSystemName) const;

	flecs::entity CreatePersistentEntity(const char* InName = nullptr);
	flecs::entity CreateWorldEntity(const char* InName = nullptr);

	template <typename... Components, typename FuncType>
	flecs::entity RegisterPersistentOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		if (!EcsWorld.IsValid() || SystemName.IsNone())
		{
			return flecs::entity();
		}

		UnregisterPersistentSystem(SystemName);

		FTCHARToUTF8 convertedName(*SystemName.ToString());
		flecs::entity systemEntity = EcsWorld->system<Components...>(convertedName.Get())
			.kind(flecs::OnUpdate)
			.each(Forward<FuncType>(Func));

		PersistentSystems.Add(SystemName, systemEntity);
		return systemEntity;
	}

	bool UnregisterPersistentSystem(FName SystemName);
	bool UnregisterPersistentSystem(flecs::entity SystemEntity);

private:
	void DestroyActiveWorldScope();

	TUniquePtr<flecs::world> EcsWorld;
	TWeakObjectPtr<UWorld> ActiveWorld;
	flecs::entity ActiveWorldScope;
	TMap<FName, flecs::entity> PersistentSystems;
	uint64 LastProgressFrame = MAX_uint64;
	uint64 WorldGeneration = 0;
	bool bIsProgressing = false;
	bool bIsShuttingDown = false;
};
