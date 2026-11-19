#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
enum class EStateOperation : uint8
	{
		EntityCreate,
		ComponentUpsert,
		ComponentRemove,
		EntityDespawn,
		EntityLeaveRelevance
	};
}
