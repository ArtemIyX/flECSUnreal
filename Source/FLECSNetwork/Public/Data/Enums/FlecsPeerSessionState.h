#pragma once

#include "CoreMinimal.h"

enum class EFlecsPeerSessionState : uint8
{
	AwaitingChannel,
	AwaitingManifest,
	Compatible,
	Rejected,
	Closing
};
