#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsConnectionIncarnation.h"

class APlayerController;
class AFlecsReplicationChannel;

/** Game-thread context only; never capture its controller in worker callbacks. */
struct FFlecsPeerSpawnContext
{
	FConnectionIncarnation Incarnation;
	uint64 ServerWorldEpoch = 0;
	TWeakObjectPtr<APlayerController> Controller;
	FVector ViewPosition = FVector::ZeroVector;
};
