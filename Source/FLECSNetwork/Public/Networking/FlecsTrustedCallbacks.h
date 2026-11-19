#pragma once

#include "CoreMinimal.h"

/** Worker-safe native permission callback; it must not access UObjects. */
using FFlecsTrustedPermissionRule = TFunction<bool(uint64 ConnectionToken, uint64 OwnerConnectionToken,
	uint32 NetworkEntityId, uint16 Generation)>;
using FFlecsTrustedPositionAccessor = TFunction<bool(uint64 EntityToken, FVector3d& OutPosition)>;
