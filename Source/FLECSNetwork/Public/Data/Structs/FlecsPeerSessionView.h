#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsConnectionIncarnation.h"
#include "Data/Enums/FlecsPeerSessionState.h"

class APlayerController;
class AFlecsReplicationChannel;

/** Game-thread value view; local attachment and server network epochs are distinct. */
struct FFlecsPeerSessionView
{
	FConnectionIncarnation Incarnation;
	EFlecsPeerSessionState State = EFlecsPeerSessionState::AwaitingChannel;
	uint64 LocalAttachmentEpoch = 0;
	uint64 ServerWorldEpoch = 0;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<AFlecsReplicationChannel> Channel;
	FString RejectionReason;
};
