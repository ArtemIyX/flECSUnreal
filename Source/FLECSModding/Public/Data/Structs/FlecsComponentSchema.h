#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsModVersion.h"
#include "Data/Enums/FlecsModVisibilityPolicy.h"

namespace FlecsModRegistry
{
struct FComponentSchema
{
	FString StableId;
	FVersion Version;
	uint32 MaxEncodedBytes = 0;
	TArray<FString> CompatibleWith;
	/** Empty permits any definition that includes this component; otherwise this is an allowlist. */
	TArray<FString> AllowedEntityDefinitions;
	EVisibilityPolicy Visibility = EVisibilityPolicy::Relevant;
	float UpdateRateHz = 15.0f;
	int32 Priority = 0;
	// These callbacks execute on the owning simulation worker. They receive only
	// opaque entity tokens and bounded encoded bytes; they must not access UObjects.
	TFunction<bool(uint64 EntityToken, TArray<uint8>& OutBytes)> Extract;
	TFunction<bool(uint64 EntityToken, TConstArrayView<uint8> Bytes)> Apply;
	TFunction<void(uint64 EntityToken)> Remove;
};
}
