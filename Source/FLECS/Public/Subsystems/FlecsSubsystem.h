#pragma once

#include "CoreMinimal.h"
#include "Containers/StringConv.h"
#include "Engine/EngineTypes.h"
#include "Tickable.h"
#include "Subsystems/WorldSubsystem.h"
#include "Misc/Build.h"

PRAGMA_DISABLE_UNREACHABLE_CODE_WARNINGS
#include "flecs.h"


#include "FlecsSubsystem.generated.h"

UCLASS(Blueprintable, BlueprintType)
class FLECS_API UFlecsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UFlecsSubsystem();
	virtual ~UFlecsSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	flecs::world* GetEcsWorld();
	const flecs::world* GetEcsWorld() const;

	bool UnregisterSystem(FName SystemName);
	bool UnregisterSystem(flecs::entity SystemEntity);
	void Tick(float DeltaTime);

	template <typename... Components, typename FuncType>
	flecs::entity RegisterOnUpdateSystem(const FName SystemName, FuncType&& Func)
	{
		check(EcsWorld.IsValid());
		check(!SystemName.IsNone());

		UnregisterSystem(SystemName);

		FTCHARToUTF8 ConvertedName(*SystemName.ToString());
		flecs::entity SystemEntity = EcsWorld->system<Components...>(ConvertedName.Get())
			.kind(flecs::OnUpdate)
			.each(Forward<FuncType>(Func));

		RuntimeSystems.Add(SystemName, SystemEntity);
		return SystemEntity;
	}

private:
	ETickingGroup ResolveTickGroup() const;

	struct FFlecsTickFunction : public FTickFunction
	{
		UFlecsSubsystem* Owner = nullptr;
		virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent) override;
		virtual FString DiagnosticMessage() override;
	};

	FFlecsTickFunction TickFunction;
	TUniquePtr<flecs::world> EcsWorld;
	TMap<FName, flecs::entity> RuntimeSystems;
};
