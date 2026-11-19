#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsScheduledEntity.h"
#include "Data/Structs/FlecsRelevanceMetrics.h"

struct FLECSNETWORK_API FFlecsRelevanceSelection
{
	uint64 SelectionId = 0;
	TArray<FFlecsScheduledEntity> Records;
	FFlecsRelevanceMetrics Metrics;
};
