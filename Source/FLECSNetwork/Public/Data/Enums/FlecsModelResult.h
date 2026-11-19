#pragma once

#include "CoreMinimal.h"

namespace FlecsSimulation
{
enum class EModelResult : uint8
{
	Accepted,
	Partial,
	Invalid,
	WrongRole,
	WrongEpoch,
	StaleIdentity,
	StaleTick,
	UnknownSchema,
	Unauthorized,
	RateLimited,
	Replay,
	LimitReached,
	CallbackFailed,
	/** A callback left mutation state uncertain; the bridge must discard/reset this session. */
	SessionInvalidated,
	/** No delta was staged because the peer's batch or retained baseline is full. */
	CapacityDeferred
};
}
