#pragma once
#include "CoreMinimal.h"
#include "Data/Structs/FlecsReplicationRecord.h"

/** Last accepted logical state batch. Transport revisions and transfer IDs are ignored. */
struct FLECSNETWORK_API FFlecsLogicalStateBatch
{
	uint64 Epoch = 0;
	uint32 Tick = 0;
	TArray<FFlecsReplicationRecord> Records;
	bool Accept(uint64 InEpoch, uint32 InTick, const TArray<FFlecsReplicationRecord>& InRecords)
	{
		bool bSame = Epoch == InEpoch && Tick == InTick && Records.Num() == InRecords.Num();
		for (int32 Index = 0; bSame && Index < Records.Num(); ++Index)
		{
			const auto& A = Records[Index];
			const auto& B = InRecords[Index];
			bSame = A.WorldEpoch == B.WorldEpoch && A.NetworkEntityId == B.NetworkEntityId
				 && A.EntityGeneration == B.EntityGeneration && A.DefinitionWireId == B.DefinitionWireId
				 && A.ComponentWireId == B.ComponentWireId && A.SourceSimulationTick == B.SourceSimulationTick
				 && A.Operation == B.Operation && A.Payload == B.Payload;
		}
		if (bSame)
			return false;
		Epoch = InEpoch;
		Tick = InTick;
		Records = InRecords;
		return true;
	}
};
