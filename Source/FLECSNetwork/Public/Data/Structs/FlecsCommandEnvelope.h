#pragma once

#include "CoreMinimal.h"
#include "FlecsBoundedPayload.h"
#include "FlecsCommandEnvelope.generated.h"

USTRUCT()
struct FLECSNETWORK_API FFlecsCommandEnvelope
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;
	UPROPERTY()
	uint32 Sequence = 0;
	UPROPERTY()
	uint32 TargetNetworkEntityId = 0;
	UPROPERTY()
	uint16 TargetEntityGeneration = 0;
	UPROPERTY()
	uint32 RequestedSimulationTick = 0;
	UPROPERTY()
	uint16 CommandWireId = 0;
	UPROPERTY()
	FFlecsBoundedPayload Payload;
};
