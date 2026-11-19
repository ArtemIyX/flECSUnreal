#pragma once

#include "CoreMinimal.h"
#include "Iris/ReplicationState/IrisFastArraySerializer.h"
#include "Data/Structs/FlecsReplicationRecord.h"
#include "FlecsReplicationRecordArray.generated.h"

class AFlecsReplicationChannel;

USTRUCT()
struct FLECSNETWORK_API FFlecsReplicationRecordArray : public FIrisFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FFlecsReplicationRecord> Items;

	/** Set by the owning actor; replication callbacks remain on Unreal's game thread. */
	AFlecsReplicationChannel* Owner = nullptr;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FFlecsReplicationRecord, FFlecsReplicationRecordArray>(
			Items, DeltaParms, *this);
	}

	void PostReplicatedReceive(const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters);
};

template <>
struct TStructOpsTypeTraits<FFlecsReplicationRecordArray> : TStructOpsTypeTraitsBase2<FFlecsReplicationRecordArray>
{
	enum
	{
		WithNetDeltaSerializer = true
	};
};
