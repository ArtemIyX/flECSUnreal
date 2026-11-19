#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsModVersion.h"

namespace FlecsModRegistry
{
struct FEntityDefinition
{
	FString StableId;
	FVersion Version;
	TArray<FString> ComponentIds;
	uint32 MaxInitialStateBytes = 0;
	TFunction<bool(uint64 EntityToken)> Create;
};
}
