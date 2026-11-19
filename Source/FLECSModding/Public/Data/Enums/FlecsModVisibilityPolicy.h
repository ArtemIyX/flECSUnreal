#pragma once

#include "CoreMinimal.h"

namespace FlecsModRegistry
{
enum class EVisibilityPolicy : uint8
{
	Relevant,
	OwnerOnly,
	Always
};
}
