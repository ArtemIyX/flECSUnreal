#pragma once

#include "CoreMinimal.h"

enum class EFlecsTrustedSpatialPolicy : uint8
{
	PositionRequired,
	AlwaysRelevantWithoutPosition,
	SkipWithoutPosition
};
