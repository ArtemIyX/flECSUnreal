#pragma once

#include "CoreMinimal.h"
#include "FlecsDistanceUpdateRate.generated.h"

/** Replication frequency for entities up to this distance, in Unreal centimeters. */
USTRUCT(BlueprintType)
struct FLECS_API FFlecsDistanceUpdateRate
{
	GENERATED_BODY()

	FFlecsDistanceUpdateRate() = default;
	FFlecsDistanceUpdateRate(float InMaxDistance, float InUpdateRateHz, FColor InDebugColor = FColor::White)
		: MaxDistance(InMaxDistance)
		, UpdateRateHz(InUpdateRateHz)
		, DebugColor(InDebugColor)
	{
	}

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Relevance",
		meta = (ClampMin = "1.0", ClampMax = "1000000.0", Units = "cm"))
	float MaxDistance = 5000.0f;

	/** Maximum replication updates per second, not the simulation tick rate. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Relevance",
		meta = (ClampMin = "0.01", ClampMax = "60.0", Units = "Hz"))
	float UpdateRateHz = 30.0f;

	/** Color of the client-side estimated distance tier. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Relevance")
	FColor DebugColor = FColor::White;
};
