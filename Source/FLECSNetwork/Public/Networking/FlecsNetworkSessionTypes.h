#pragma once

#include "CoreMinimal.h"

class APlayerController;
class AFlecsReplicationChannel;

/** Worker-safe native permission callback; it must not access UObjects. */
using FFlecsTrustedPermissionRule = TFunction<bool(uint64 ConnectionToken, uint64 OwnerConnectionToken,
	uint32 NetworkEntityId, uint16 Generation)>;
using FFlecsTrustedPositionAccessor = TFunction<bool(uint64 EntityToken, FVector3d& OutPosition)>;

enum class EFlecsTrustedSpatialPolicy : uint8
{
	PositionRequired,
	AlwaysRelevantWithoutPosition,
	SkipWithoutPosition
};

struct FFlecsTrustedSpatialDefinition
{
	EFlecsTrustedSpatialPolicy Policy = EFlecsTrustedSpatialPolicy::PositionRequired;
	FFlecsTrustedPositionAccessor PositionAccessor;
};

/** Opaque identity for one observed player connection incarnation. */
struct FConnectionIncarnation
{
	uint64 Value = 0;

	bool IsValid() const { return Value != 0; }
	bool operator==(const FConnectionIncarnation& Other) const { return Value == Other.Value; }
};

enum class EFlecsPeerSessionState : uint8
{
	AwaitingChannel,
	AwaitingManifest,
	Compatible,
	Rejected,
	Closing
};

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
