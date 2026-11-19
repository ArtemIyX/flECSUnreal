#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsStateRecord.h"

namespace FlecsSimulation
{
struct FStateBatch
	{
		uint64 ServerWorldEpoch = 0;
		uint32 SimulationTick = 0;
		/** Worker-issued token used to commit or discard one per-peer delta baseline. */
		uint64 PublicationRevision = 0;
		bool bCompleteStateSnapshot = false;
		TArray<FStateRecord> Records;
	};
}
