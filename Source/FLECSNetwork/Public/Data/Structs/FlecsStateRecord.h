#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"
#include "Data/Enums/FlecsStateOperation.h"

namespace FlecsSimulation
{
/** Owned DTO record. Component payloads are versioned by the peer's session mapping. */
	struct FStateRecord
	{
		FNetworkEntityIdentity Identity;
		uint16 DefinitionWireId = 0;
		uint16 ComponentWireId = 0;
		uint32 SourceSimulationTick = 0;
		EStateOperation Operation = EStateOperation::EntityCreate;
		TArray<uint8> Payload;
	};
}
