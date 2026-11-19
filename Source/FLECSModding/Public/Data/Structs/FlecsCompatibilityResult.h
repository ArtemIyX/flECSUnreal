#pragma once

#include "CoreMinimal.h"
#include "ModRegistry/FlecsSessionMapping.h"

namespace FlecsModRegistry
{
struct FCompatibilityResult
	{
		bool bCompatible = false;
		TArray<FString> Diagnostics;
		FSessionMapping SessionMapping;
	};
}
