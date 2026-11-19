#pragma once

#include "CoreMinimal.h"
#include "ModRegistry/ModSchemaRegistry.h"
#include "Networking/FlecsTrustedCallbacks.h"
#include "Networking/FlecsSessionCallbacks.h"
#include "Data/Enums/FlecsTrustedSpatialPolicy.h"

/** Synchronous game-thread registration hooks. Do not retain these callbacks after setup. */
struct FFlecsNetworkSessionRegistrar
{
	FlecsModRegistry::FRegistry& Registry;
	TFunctionRef<bool(const FString&, FFlecsTrustedPermissionRule, FString&)> RegisterPermission;
	TFunctionRef<bool(const FString&, EFlecsTrustedSpatialPolicy, FFlecsTrustedPositionAccessor, FString&)>
		RegisterSpatial;
	TFunctionRef<bool(FFlecsPeerSpawnFactory, FString&)> SetPeerSpawnFactory;
	/** Optional setter; configured providers run on the game thread before each selection. */
	TFunction<bool(FFlecsPeerInterestProvider, FString&)> SetPeerInterestProvider;
};
