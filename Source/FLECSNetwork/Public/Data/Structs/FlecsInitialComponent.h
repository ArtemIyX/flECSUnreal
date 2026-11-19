#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
struct FInitialComponent
{
	FString StableComponentId;
	TArray<uint8> Payload;
};
}
