#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsSpatialEntity.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"

/**
 * One immutable, bounded simulation publication used by spatial selection.
 * The caller keeps this object alive and unchanged while its spatial index is used.
 */
struct FLECSNETWORK_API FFlecsSpatialSnapshot
{
	uint64 WorldEpoch = 0;
	uint64 SimulationTick = 0;
	TArray<FFlecsSpatialEntity> Entities;
	/** Complete worker-owned identity list, independent of partial spatial metadata. */
	TArray<FFlecsNetworkEntityKey> LiveEntityKeys;
	/** When true, only LiveEntityKeys may establish entity absence or generation retirement. */
	bool bHasCompleteLiveEntitySet = false;
};
