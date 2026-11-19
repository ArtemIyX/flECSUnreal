#pragma once

#include "CoreMinimal.h"
#include "FlecsServerTickHeartbeat.generated.h"

USTRUCT()
struct FLECSNETWORK_API FFlecsServerTickHeartbeat
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 ServerWorldEpoch = 0;

	UPROPERTY()
	uint32 SimulationTick = 0;
};
