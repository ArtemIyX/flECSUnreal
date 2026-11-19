#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"
#include "Data/Enums/FlecsNetworkVisibility.h"

/** Complete spatial/revision metadata copied at one simulation publication boundary. */
struct FLECSNETWORK_API FFlecsSpatialEntity
{
	FFlecsNetworkEntityKey Key;
	FVector3f Position = FVector3f::ZeroVector;
	uint64 Revision = 0;
	uint64 OwnerConnectionId = 0;
	uint32 EstimatedUpdateBytes = 0;
	/** Optional schema-derived cap for the entity cadence; zero uses its distance tier unchanged. */
	float RecommendedUpdateRateHz = 0.0f;
	int32 Importance = 0;
	EFlecsNetworkVisibility Visibility = EFlecsNetworkVisibility::RelevantToAll;
	bool bAlwaysRelevant = false;
};
