#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"
#include "Data/Structs/FlecsRelevanceTier.h"

struct FLECSNETWORK_API FFlecsConnectionInterest
{
	uint64 ConnectionId = 0;
	FVector3f ViewPosition = FVector3f::ZeroVector;
	/** Per-connection explicit show/hide overrides; absent keys use distance/visibility policy. */
	TMap<FFlecsNetworkEntityKey, bool> VisibilityOverrides;
	TArray<FFlecsRelevanceTier> Tiers;
	float HysteresisDistance = 250.0f;
};
