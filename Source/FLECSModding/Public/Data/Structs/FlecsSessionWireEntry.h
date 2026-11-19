#pragma once

#include "CoreMinimal.h"
#include "Data/Enums/FlecsModEntryKind.h"

namespace FlecsModRegistry
{
struct FSessionWireEntry
{
	EEntryKind Kind = EEntryKind::Component;
	FString StableId;
};
}
