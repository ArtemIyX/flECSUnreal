#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsPeerSpawnContext.h"
#include "Data/Structs/FlecsPeerSpawnRequest.h"
#include "Relevance/FlecsRelevanceScheduler.h"

/** Game-thread factory. Return false with a reason to reject a peer's spawn request. */
using FFlecsPeerSpawnFactory = TFunction<bool(const FFlecsPeerSpawnContext& Context,
	FFlecsPeerSpawnRequest& OutRequest, FString& OutError)>;

/** Game-thread override of view, tiers, visibility, and bounded work for one peer. */
using FFlecsPeerInterestProvider = TFunction<bool(const FFlecsPeerSpawnContext& Context,
	FFlecsConnectionInterest& InOutInterest, FFlecsRelevanceBudget& InOutBudget, FString& OutError)>;
