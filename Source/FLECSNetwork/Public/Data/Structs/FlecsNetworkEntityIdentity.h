#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
/** Server-assigned network identity; it never contains a local Flecs entity ID. */
struct FNetworkEntityIdentity
{
	uint64 ServerWorldEpoch = 0;
	uint32 NetworkEntityId = 0;
	uint16 Generation = 0;
	FString DefinitionStableId;

	bool IsValid() const
	{
		return ServerWorldEpoch != 0 && NetworkEntityId != 0 && Generation != 0 && !DefinitionStableId.IsEmpty();
	}
};
}
