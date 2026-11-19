#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
struct FClientCommand
{
	uint64 ServerWorldEpoch = 0;
	uint64 TrustedConnectionToken = 0; // assigned from the owning server endpoint, never copied from client bytes
	uint32 Sequence = 0;
	uint32 TargetNetworkEntityId = 0;
	uint16 TargetGeneration = 0;
	uint32 RequestedSimulationTick = 0;
	uint16 CommandWireId = 0;
	bool bDiscrete = true;
	TArray<uint8> Payload;
};
}
