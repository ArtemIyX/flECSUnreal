#pragma once

#include "CoreMinimal.h"

enum class EFlecsWorkerCommandResult : uint8
{
	Accepted,
	InvalidCommand,
	StaleEpoch,
	QueueFull,
	Stopping,
	RegistryFull
};
