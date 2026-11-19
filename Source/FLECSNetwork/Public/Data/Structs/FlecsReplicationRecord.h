#pragma once

#include "CoreMinimal.h"
#include "FlecsBoundedPayload.h"
#include "Iris/ReplicationState/IrisFastArraySerializer.h"
#include "FlecsReplicationRecord.generated.h"

/** A bounded, versioned state record. Identity values are assigned by the server, never by local Flecs IDs. */
USTRUCT()
struct FLECSNETWORK_API FFlecsReplicationRecord : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;

	UPROPERTY()
	uint32 NetworkEntityId = 0;

	UPROPERTY()
	uint16 EntityGeneration = 0;

	UPROPERTY()
	uint16 DefinitionWireId = 0;

	UPROPERTY()
	uint16 ComponentWireId = 0;

	UPROPERTY()
	uint32 SourceSimulationTick = 0;

	/** 0 = upsert, 1 = component removal, 2 = true despawn, 3 = entity create, 4 = leave relevance. */
	UPROPERTY()
	uint8 Operation = 0;

	UPROPERTY()
	uint64 SnapshotRevision = 0;

	UPROPERTY()
	uint32 SnapshotTransferId = 0;

	UPROPERTY()
	uint16 SnapshotRecordIndex = 0;

	UPROPERTY()
	uint16 SnapshotRecordCount = 0;

	UPROPERTY()
	uint32 SnapshotSimulationTick = 0;

	UPROPERTY()
	FFlecsBoundedPayload Payload;
};
