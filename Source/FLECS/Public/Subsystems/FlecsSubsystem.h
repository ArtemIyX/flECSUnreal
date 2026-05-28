// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Containers/StringConv.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "Misc/Build.h"

PRAGMA_DISABLE_UNREACHABLE_CODE_WARNINGS
#include "flecs.h"

#include "FlecsSubsystem.generated.h"

UCLASS(Blueprintable, BlueprintType)
class FLECS_API UFlecsSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UFlecsSubsystem();
	virtual ~UFlecsSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	flecs::world* GetEcsWorld();
	const flecs::world* GetEcsWorld() const;

	bool UnregisterSystem(FName SystemName);
	bool UnregisterSystem(flecs::entity SystemEntity);
	bool Tick(float DeltaTime);

	template <typename... Components, typename FuncType>
	flecs::entity RegisterOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		check(EcsWorld.IsValid());
		check(!SystemName.IsNone());

		UnregisterSystem(SystemName);

		FTCHARToUTF8 convertedName(*SystemName.ToString());
		flecs::entity systemEntity = EcsWorld->system<Components...>(convertedName.Get())
			.kind(flecs::OnUpdate)
			.each(Forward<FuncType>(Func));

		RuntimeSystems.Add(SystemName, systemEntity);
		return systemEntity;
	}

private:
	FTSTicker::FDelegateHandle TickHandle;
	TUniquePtr<flecs::world> EcsWorld;
	TMap<FName, flecs::entity> RuntimeSystems;
};
