#pragma once

#include "CoreMinimal.h"

enum class EFlecsSessionEventKind : uint8
{
	Compatible,
	Rejected,
	PeerSpawnFinished,
	SharedSpawnFinished,
	ClientStateApplied,
	ServerTransferStarted,
	ServerTransferAcknowledged,
	ServerCommandFinished,
	ClientCommandResult
};
