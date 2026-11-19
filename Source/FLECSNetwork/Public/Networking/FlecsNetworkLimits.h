#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsRelevanceBudget.h"

class UFlecsDeveloperSettings;

namespace FlecsNetworkLimits
{
inline constexpr int32 MaxEntityCapacity = 100000;
inline constexpr int32 MaxIdentityHistory = 1000000;
}

/** Value-only settings captured once on the game thread for one network session. */
struct FLECSNETWORK_API FFlecsNetworkLimits
{
	int32 MaxEntities = 5000;
	int32 MaxOwnedEntitiesPerPeer = 256;
	int32 MaxRelevantEntitiesPerPeer = 5000;
	uint32 MaxRecordsPerSelection = 128;
	uint32 MaxPayloadBytesPerSelection = 64 * 1024;

	FFlecsRelevanceBudget MakeSelectionBudget() const
	{
		FFlecsRelevanceBudget Budget;
		Budget.MaxRecords = MaxRecordsPerSelection;
		Budget.MaxBytes = MaxPayloadBytesPerSelection;
		return Budget;
	}

	/** Returns false if any input was clamped; OutLimits always contains usable values. */
	static bool Capture(const UFlecsDeveloperSettings& Settings, FFlecsNetworkLimits& OutLimits);
};
