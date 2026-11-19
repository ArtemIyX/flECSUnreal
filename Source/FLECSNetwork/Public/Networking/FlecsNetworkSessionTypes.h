#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsConnectionIncarnation.h"
#include "Data/Structs/FlecsPeerSpawnContext.h"
#include "Data/Structs/FlecsPeerSpawnRequest.h"
#include "Data/Structs/FlecsTrustedSpatialDefinition.h"
#include "Data/Structs/FlecsNetworkSessionRegistrar.h"
#include "Data/Structs/FlecsPeerSessionView.h"
#include "Data/Structs/FlecsSessionEvent.h"
#include "Data/Enums/FlecsTrustedSpatialPolicy.h"
#include "Data/Enums/FlecsPeerSessionState.h"
#include "Data/Enums/FlecsSessionEventKind.h"
#include "Networking/FlecsTrustedCallbacks.h"
#include "Networking/FlecsSessionCallbacks.h"
#include "Relevance/FlecsRelevanceScheduler.h"
#include "Simulation/FlecsWorkerEntityModel.h"

class APlayerController;
class AFlecsReplicationChannel;
class UWorld;

/** Called once per world before registry freeze; register only trusted worker-safe callbacks. */
using FFlecsNetworkSessionConfigurator =
	TFunction<bool(UWorld& World, FFlecsNetworkSessionRegistrar& Registrar, FString& OutError)>;

DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsSessionEventDelegate, const FFlecsSessionEvent&);
