#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsModVersion.h"

namespace FlecsModRegistry
{
struct FCommandSchema
	{
		FString StableId;
		FVersion Version;
		uint32 MaxPayloadBytes = 0;
		FString RequiredPermission;
		// Optional worker-only preflight; must not mutate state. False rejects the command.
		TFunction<bool(uint64 ConnectionToken, uint64 TargetToken, TConstArrayView<uint8> Payload)> Validate;
		// A false result may follow mutation and therefore invalidates the authoritative session.
		TFunction<bool(uint64 ConnectionToken, uint64 TargetToken, TConstArrayView<uint8> Payload)> Execute;
	};
}
