#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"

struct FLECSNETWORK_API FFlecsRelevanceMetrics
{
	uint32 CandidateCount = 0;
	uint32 SelectedRecords = 0;
	uint32 SelectedBytes = 0;
	uint32 DeferredByBudget = 0;
	uint32 OversizedEntityCount = 0;
	FFlecsNetworkEntityKey FirstOversizedEntity;
	uint32 StaleSnapshotRecords = 0;
	uint32 CurrentlyRelevant = 0;
	uint64 OldestDeferredAgeMs = 0;
};
