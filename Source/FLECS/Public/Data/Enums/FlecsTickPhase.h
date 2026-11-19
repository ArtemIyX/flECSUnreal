#pragma once

#include "CoreMinimal.h"
#include "FlecsTickPhase.generated.h"

UENUM(BlueprintType)
enum class EFlecsTickPhase : uint8
{
	PrePhysics UMETA(DisplayName = "Pre Physics"),
	DuringPhysics UMETA(DisplayName = "During Physics"),
	PostPhysics UMETA(DisplayName = "Post Physics"),
	PostUpdateWork UMETA(DisplayName = "Post Update Work")
};
