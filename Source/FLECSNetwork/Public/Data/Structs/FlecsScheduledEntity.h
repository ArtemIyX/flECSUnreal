#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"
#include "Data/Enums/FlecsRelevanceTransition.h"

struct FLECSNETWORK_API FFlecsScheduledEntity
{
	FFlecsNetworkEntityKey Key;
	EFlecsRelevanceTransition Transition = EFlecsRelevanceTransition::Update;
	uint64 Revision = 0;
	uint64 SimulationTick = 0;
	uint32 EstimatedBytes = 0;
	uint64 OwnerConnectionId = 0;
	uint8 Tier = 0;
	bool bEntityDespawned = false;
};
