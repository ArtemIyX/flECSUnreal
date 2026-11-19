#pragma once

#include "CoreMinimal.h"
#include "Networking/FlecsTrustedCallbacks.h"
#include "Data/Enums/FlecsTrustedSpatialPolicy.h"

struct FFlecsTrustedSpatialDefinition
{
	EFlecsTrustedSpatialPolicy Policy = EFlecsTrustedSpatialPolicy::PositionRequired;
	FFlecsTrustedPositionAccessor PositionAccessor;
};
