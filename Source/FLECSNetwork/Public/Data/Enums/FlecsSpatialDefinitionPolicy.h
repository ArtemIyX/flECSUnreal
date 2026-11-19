#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
enum class ESpatialDefinitionPolicy : uint8
	{
		PositionRequired,
		AlwaysRelevantWithoutPosition,
		SkipWithoutPosition
	};
}
