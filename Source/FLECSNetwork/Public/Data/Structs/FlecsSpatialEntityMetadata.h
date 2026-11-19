#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"

namespace FlecsSimulation
{
/** Copied scheduler input; contains no component payloads or Flecs handles. */
	struct FSpatialEntityMetadata
	{
		FNetworkEntityIdentity Identity;
		FVector3d Position = FVector3d::ZeroVector;
		/** Monotonic worker-owned entity state revision, independent of the global fixed-step tick. */
		uint64 Revision = 0;
		uint64 OwnerConnectionToken = 0;
		int32 Importance = 0;
		uint32 EstimatedUpdateBytes = 0;
		float RecommendedUpdateRateHz = 0.0f;
		bool bAlwaysRelevant = false;
	};
}
