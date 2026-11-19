#pragma once

#include "CoreMinimal.h"
#include "../../ThirdParty/flecs/Include/flecs.h"

#include <functional>

/** Immutable worker status copied for consumers; contains no Flecs references. */
struct FFlecsSimulationPublication
{
	uint64 Epoch = 0;
	uint64 Tick = 0;
	uint64 Steps = 0;
	uint64 MissedDeadlines = 0;
	uint64 RejectedCommands = 0;
	uint64 CommandsAppliedLastStep = 0;
	double LastStepSeconds = 0.0;
	double MaxStepSeconds = 0.0;
	double LastDeadlineLatenessSeconds = 0.0;
	int32 QueueDepth = 0;
	double OldestCommandAgeSeconds = 0.0;
};

using FFlecsWorkerCommand = std::function<void(flecs::world&)>;

enum class EFlecsWorkerCommandResult : uint8
{
	Accepted,
	InvalidCommand,
	StaleEpoch,
	QueueFull,
	Stopping
};
