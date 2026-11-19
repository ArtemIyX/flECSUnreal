#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"

namespace FlecsSimulation
{
/** Scheduler-selected entity update; empty component list means all visible components are due when the flag is true. */
	struct FSelectedEntityComponents
	{
		FNetworkEntityIdentity Identity;
		TArray<FString> DueComponentStableIds;
		bool bAllVisibleComponentsDue = true;
	};
}
