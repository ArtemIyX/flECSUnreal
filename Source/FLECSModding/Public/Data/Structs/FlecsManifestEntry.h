#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsModVersion.h"
#include "Data/Enums/FlecsModEntryKind.h"

namespace FlecsModRegistry
{
struct FManifestEntry
{
	EEntryKind Kind = EEntryKind::Component;
	FString StableId;
	FVersion Version;
	uint32 MaxPayloadBytes = 0;
	FString Metadata;
};
}
