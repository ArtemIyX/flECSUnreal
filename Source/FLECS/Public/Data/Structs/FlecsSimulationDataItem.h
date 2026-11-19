#pragma once

#include "CoreMinimal.h"

struct FFlecsSimulationDataItem
{
	/** Nonzero application-defined schema/type identifier. */
	uint32 TypeId = 0;
	/** Owned copy of the encoded application DTO. */
	TArray<uint8> Payload;
};
