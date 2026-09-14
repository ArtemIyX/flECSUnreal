#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Containers/StringConv.h"
#include "Misc/Build.h"
#include "FlecsEntityHandle.h"

PRAGMA_DISABLE_UNREACHABLE_CODE_WARNINGS
#include "flecs.h"

#include "FlecsGameInstanceSubsystem.generated.h"

class UWorld;

DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsWorldLifecycleEvent, UWorld&);

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
	uint64 GetWorldSerial() const { return WorldSerial; }

	FFlecsEntityHandle MakeEntityHandle(ecs_entity_t InEntity) const;
	bool ResolveEntityHandle(const FFlecsEntityHandle& InHandle, ecs_entity_t& OutEntity) const;
	bool IsEntityInScope(ecs_entity_t InEntity, flecs::entity InScope) const;

	bool AttachWorld(UWorld& InWorld);
	void DetachWorld(UWorld& InWorld);
	bool IsWorldReady(const UWorld& InWorld) const;
	bool ProgressFromWorld(UWorld& InWorld, float DeltaTime);
	FName MakeWorldSystemName(FName InSystemName) const;

	FFlecsWorldLifecycleEvent OnWorldAboutToDetach;
	FFlecsWorldLifecycleEvent OnWorldAttached;

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
	uint64 WorldSerial = 0;
	bool bIsProgressing = false;
	bool bIsShuttingDown = false;
};
