#pragma once

#include "CoreMinimal.h"
#include "Networking/FlecsNetworkLimits.h"

/** Versioned session limits followed by the serialized mod manifest. */
struct FLECSNETWORK_API FFlecsSessionManifestEnvelope
{
	static constexpr uint32 MaxNetworkEntities = static_cast<uint32>(FlecsNetworkLimits::MaxEntityCapacity);
	static constexpr int32 HeaderBytes = 12;

	static bool Encode(uint32 EntityCapacity, TConstArrayView<uint8> Manifest, TArray<uint8>& OutBytes);
	static bool Decode(TConstArrayView<uint8> Bytes, uint32& OutEntityCapacity, TArray<uint8>& OutManifest);
	static bool DecodeMatching(TConstArrayView<uint8> Bytes, uint32 ExpectedCapacity,
		TArray<uint8>& OutManifest, FString& OutError);
};
