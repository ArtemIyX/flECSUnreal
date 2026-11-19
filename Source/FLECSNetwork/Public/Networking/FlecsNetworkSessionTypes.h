#pragma once

#include "CoreMinimal.h"
#include "Simulation/FlecsWorkerEntityModel.h"

class APlayerController;
class AFlecsReplicationChannel;
class UWorld;

/** Worker-safe native permission callback; it must not access UObjects. */
using FFlecsTrustedPermissionRule = TFunction<bool(uint64 ConnectionToken, uint64 OwnerConnectionToken,
	uint32 NetworkEntityId, uint16 Generation)>;
using FFlecsTrustedPositionAccessor = TFunction<bool(uint64 EntityToken, FVector3d& OutPosition)>;

/** A game-provided initial entity; the session supplies the peer's trusted owner token. */
struct FFlecsPeerSpawnRequest
{
	FString DefinitionStableId;
	TArray<FlecsSimulation::FInitialComponent> InitialComponents;
};

/** Game-thread factory. Return false with a reason to reject a peer's spawn request. */
using FFlecsPeerSpawnFactory = TFunction<bool(const FVector& ViewPosition,
	FFlecsPeerSpawnRequest& OutRequest, FString& OutError)>;

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

/** Synchronous game-thread registration hooks. Do not retain these callbacks after setup. */
struct FFlecsNetworkSessionRegistrar
{
	FlecsModRegistry::FRegistry& Registry;
	TFunctionRef<bool(const FString&, FFlecsTrustedPermissionRule, FString&)> RegisterPermission;
	TFunctionRef<bool(const FString&, EFlecsTrustedSpatialPolicy, FFlecsTrustedPositionAccessor, FString&)> RegisterSpatial;
	TFunctionRef<bool(FFlecsPeerSpawnFactory, FString&)> SetPeerSpawnFactory;
};

/** Called once per world before registry freeze; register only trusted worker-safe callbacks. */
using FFlecsNetworkSessionConfigurator = TFunction<bool(UWorld& World,
	FFlecsNetworkSessionRegistrar& Registrar, FString& OutError)>;

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
