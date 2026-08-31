#pragma once

#include "CoreMinimal.h"
#include "Containers/StringConv.h"
#include "Engine/EngineTypes.h"
#include "Tickable.h"
#include "Subsystems/WorldSubsystem.h"
#include "Misc/Build.h"
#include "Subsystems/FlecsGameInstanceSubsystem.h"

#pragma push_macro("FLECS_API")
#undef FLECS_API

PRAGMA_DISABLE_UNREACHABLE_CODE_WARNINGS
#include "flecs.h"

#pragma pop_macro("FLECS_API")

#include "FlecsSubsystem.generated.h"

/**
 * @brief World-scoped bridge between Unreal Engine and a Flecs world.
 *
 * World-scoped bridge for the GameInstance-owned Flecs world.
 */
UCLASS(Blueprintable, BlueprintType)
class FLECS_API UFlecsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UFlecsSubsystem();

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	

public:
	/** @brief Get mutable access to the owned Flecs world. */
	flecs::world* GetEcsWorld();
	/** @brief Get const access to the owned Flecs world. */
	const flecs::world* GetEcsWorld() const;
	flecs::entity CreatePersistentEntity(const char* InName = nullptr);
	flecs::entity CreateWorldEntity(const char* InName = nullptr);

	/**
	 * @brief Unregister and destroy a runtime system by name.
	 * @param SystemName Name used during registration.
	 * @return `true` when a system was found and removed.
	 */
	bool UnregisterSystem(FName SystemName);
	/**
	 * @brief Unregister and destroy a runtime system by Flecs entity handle.
	 * @param SystemEntity Flecs entity representing a system.
	 * @return `true` when the entity was valid and removed.
	 */
	bool UnregisterSystem(flecs::entity SystemEntity);
	/**
	 * @brief Advance the Flecs world by one frame.
	 * @param DeltaTime Frame delta in seconds from Unreal tick.
	 */
	void Tick(float DeltaTime);

	/**
	 * @brief Register or replace an `OnUpdate` system bound to this subsystem world.
	 *
	 * If a system with the same name exists, it is removed before creating the new
	 * one. The returned Flecs entity can be stored and later removed via
	 * `UnregisterSystem`.
	 *
	 * @tparam Components Query component types used by the system.
	 * @tparam FuncType Callable signature accepted by `flecs::system::each`.
	 * @param SystemName Stable system name used for replacement and lookup.
	 * @param Func System callback.
	 * @return Flecs entity handle of the registered system.
	 */
	template <typename... Components, typename FuncType>
	flecs::entity RegisterOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		UFlecsGameInstanceSubsystem* ownerSubsystem = GetOwnerSubsystem();
		if (!ownerSubsystem || SystemName.IsNone())
		{
			return flecs::entity();
		}

		UnregisterSystem(SystemName);

		const FName scopedSystemName = ownerSubsystem->MakeWorldSystemName(SystemName);
		FTCHARToUTF8 convertedName(*scopedSystemName.ToString());
		flecs::world* ecsWorld = ownerSubsystem->GetEcsWorld();
		if (!ecsWorld)
		{
			return flecs::entity();
		}

		flecs::entity systemEntity = ecsWorld->system<Components...>(convertedName.Get())
			.kind(flecs::OnUpdate)
			.each(Forward<FuncType>(Func));

		RuntimeSystems.Add(SystemName, systemEntity);
		return systemEntity;
	}

private:
	UFlecsGameInstanceSubsystem* GetOwnerSubsystem() const;
	/** @brief Resolve UE tick group from plugin developer settings. */
	ETickingGroup ResolveTickGroup() const;

	/** @brief Tick function wrapper used to integrate with UE tick scheduler. */
	struct FFlecsTickFunction : public FTickFunction
	{
		/** Owning subsystem that receives tick callbacks. */
		UFlecsSubsystem* Owner = nullptr;
		/** @brief Execute a single scheduled tick. */
		virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
		/** @brief Diagnostic string shown in UE tick debugging output. */
		virtual FString DiagnosticMessage() override;
	};

	/** UE tick function registration object for this subsystem. */
	FFlecsTickFunction TickFunction;
	/** Cached GameInstance-owned subsystem. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFlecsGameInstanceSubsystem> CachedOwnerSubsystem;
	/** Runtime systems tracked by developer-provided names. */
	TMap<FName, flecs::entity> RuntimeSystems;
};
