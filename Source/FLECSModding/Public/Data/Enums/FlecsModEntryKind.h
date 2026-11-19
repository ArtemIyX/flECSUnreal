#pragma once

#include "CoreMinimal.h"

namespace FlecsModRegistry
{
enum class EEntryKind : uint8
{
	Component,
	EntityDefinition,
	Command
};
}
