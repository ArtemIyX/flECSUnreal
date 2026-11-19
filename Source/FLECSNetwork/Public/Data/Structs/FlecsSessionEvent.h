#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsConnectionIncarnation.h"
#include "Data/Enums/FlecsSessionEventKind.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"
#include "Data/Enums/FlecsModelResult.h"

class APlayerController;
class AFlecsReplicationChannel;

/** Game-thread notification. UObject references must not be used by the worker. */
struct FFlecsSessionEvent
{
	EFlecsSessionEventKind Kind = EFlecsSessionEventKind::Compatible;
	FConnectionIncarnation Incarnation;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<AFlecsReplicationChannel> Channel;
	uint64 ServerWorldEpoch = 0;
	uint64 SnapshotRevision = 0;
	uint32 SimulationTick = 0;
	uint32 CommandSequence = 0;
	uint8 CommandResult = 0;
	FString Reason;
	uint64 SpawnRequestId = 0;
	FlecsSimulation::EModelResult SpawnResult = FlecsSimulation::EModelResult::Invalid;
	FlecsSimulation::FNetworkEntityIdentity SpawnedIdentity;
	/** Optional bounded bytes inspected on the client worker after applying this batch. */
	FlecsSimulation::FNetworkEntityIdentity InspectedIdentity;
	TArray<uint8> InspectedComponentBytes;
};
