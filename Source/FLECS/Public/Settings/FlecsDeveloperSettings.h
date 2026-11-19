#pragma once

#include "CoreMinimal.h"
#include "Data/Enums/FlecsTickPhase.h"
#include "Data/Structs/FlecsDistanceUpdateRate.h"
#include "Engine/DeveloperSettings.h"
#include "FlecsDeveloperSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="FLECS"))
class FLECS_API UFlecsDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFlecsDeveloperSettings();

	virtual FName GetCategoryName() const override
	{
		return FName(TEXT("Plugins"));
	}

	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Plugin")
	EFlecsTickPhase TickPhase = EFlecsTickPhase::PostPhysics;

	/** Fixed worker simulation steps per second. Takes effect when the world is attached. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Simulation", meta=(ClampMin="1.0", ClampMax="240.0", Units="Hz"))
	float SimulationRateHz = 60.0f;

	/** Increasing distance thresholds in centimeters and their maximum replication rates in Hz.
	 * The final distance is the relevance range. At most four tiers are supported. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Networking", meta=(TitleProperty="MaxDistance"))
	TArray<FFlecsDistanceUpdateRate> DistanceUpdateRates;
};
