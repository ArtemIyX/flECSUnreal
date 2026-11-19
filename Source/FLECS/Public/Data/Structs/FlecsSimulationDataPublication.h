#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsSimulationDataItem.h"

/** Immutable snapshot data contains copied application bytes only. Epoch identifies
 * the local Unreal world attachment; it is not a server or network-session epoch. */
struct FFlecsSimulationDataPublication
{
	uint64 LocalAttachmentEpoch = 0;
	uint64 SimulationTick = 0;
	/** Monotonic platform seconds sampled after extraction completes. */
	double TimestampSeconds = 0.0;
	uint32 TotalBytes = 0;
	uint32 RejectedItems = 0;
	TArray<FFlecsSimulationDataItem> Items;
};
