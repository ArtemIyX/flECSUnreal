#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsDistanceUpdateRate.h"
#include "Engine/DeveloperSettings.h"
#include "FlecsDeveloperSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "FLECS"))
class FLECS_API UFlecsDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFlecsDeveloperSettings();

	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }

	/** Fixed worker simulation steps per second. Takes effect when the world is attached. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Simulation",
		meta = (ClampMin = "1.0", ClampMax = "240.0", Units = "Hz"))
	float SimulationRateHz = 60.0f;

	/** Increasing distance thresholds in centimeters and their maximum replication rates in Hz.
	 * The final distance is the relevance range. At most four tiers are supported. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking", meta = (TitleProperty = "MaxDistance"))
	TArray<FFlecsDistanceUpdateRate> DistanceUpdateRates;

	/** Maximum authoritative network entities per session. Captured when the session starts. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking|Limits",
		meta = (ClampMin = "1", ClampMax = "100000"))
	int32 MaxNetworkEntities = 5000;

	/** Maximum network entities owned by one peer, also limited by MaxNetworkEntities. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking|Limits",
		meta = (ClampMin = "1", ClampMax = "100000"))
	int32 MaxPeerOwnedEntities = 256;

	/** Maximum entities retained as relevant to one peer; applied on the next session. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking|Limits",
		meta = (ClampMin = "1", ClampMax = "100000"))
	int32 MaxRelevantEntitiesPerPeer = 5000;

	/** Default maximum selected entity records per peer and selection. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking|Limits",
		meta = (ClampMin = "1", ClampMax = "16384"))
	int32 MaxRecordsPerPeerSelection = 128;

	/** Default per-peer selection budget in KiB, excluding Iris packet overhead. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Networking|Limits",
		meta = (ClampMin = "1", ClampMax = "4096", Units = "KiB"))
	int32 MaxPayloadKiBPerPeerSelection = 64;
};
