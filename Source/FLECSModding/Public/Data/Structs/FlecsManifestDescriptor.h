#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsManifestEntry.h"

namespace FlecsModRegistry
{
/** Plain, bounded manifest value suitable for serializing across a network boundary. */
struct FLECSMODDING_API FManifestDescriptor
{
	uint32 ProtocolVersion = 0;
	TArray<FManifestEntry> Entries;

	bool Validate(FString& OutError) const;
	bool Serialize(TArray<uint8>& OutBytes, FString& OutError) const;
	static bool Deserialize(TConstArrayView<uint8> Bytes, FManifestDescriptor& OutDescriptor, FString& OutError);
};
}
