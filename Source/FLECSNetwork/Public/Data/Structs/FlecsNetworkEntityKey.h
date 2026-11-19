#pragma once

#include "CoreMinimal.h"

/**
 * Network identity copied into immutable worker publications; never a local Flecs ID.
 * An entity ID is unique within one epoch; after despawn, reuse it only with a
 * strictly greater generation. Generation zero is reserved as invalid.
 */
struct FLECSNETWORK_API FFlecsNetworkEntityKey
{
	uint64 WorldEpoch = 0;
	uint32 EntityId = 0;
	uint16 Generation = 0;

	bool IsValid() const { return WorldEpoch != 0 && EntityId != 0 && Generation != 0; }
	friend bool operator==(const FFlecsNetworkEntityKey& A, const FFlecsNetworkEntityKey& B)
	{
		return A.WorldEpoch == B.WorldEpoch && A.EntityId == B.EntityId && A.Generation == B.Generation;
	}
	friend uint32 GetTypeHash(const FFlecsNetworkEntityKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.WorldEpoch), GetTypeHash(Key.EntityId)), GetTypeHash(Key.Generation));
	}
};
