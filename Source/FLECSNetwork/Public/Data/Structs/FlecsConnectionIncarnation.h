#pragma once

#include "CoreMinimal.h"

/** Opaque identity for one observed player connection incarnation. */
struct FConnectionIncarnation
{
	uint64 Value = 0;

	bool IsValid() const { return Value != 0; }
	bool operator==(const FConnectionIncarnation& Other) const { return Value == Other.Value; }
};
