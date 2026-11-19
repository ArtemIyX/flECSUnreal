#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsInitialComponent.h"

/** A game-provided initial entity; the session supplies the peer's trusted owner token. */
struct FFlecsPeerSpawnRequest
{
	FString DefinitionStableId;
	TArray<FlecsSimulation::FInitialComponent> InitialComponents;
};
