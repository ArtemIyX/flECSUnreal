#pragma once

#include "CoreMinimal.h"

/** Immutable worker status copied for consumers; contains no Flecs references. */
struct FFlecsSimulationPublication
{
	/** Local Unreal world attachment epoch. It is not a server network epoch. */
	uint64 Epoch = 0;
	uint64 Tick = 0;
	uint64 Steps = 0;
	uint64 MissedDeadlines = 0;
	uint64 RejectedCommands = 0;
	uint64 CommandsAppliedLastStep = 0;
	uint64 PublishedDataItems = 0;
	uint64 PublishedDataBytes = 0;
	uint64 RejectedDataItems = 0;
	double LastStepSeconds = 0.0;
	double MaxStepSeconds = 0.0;
	double LastDeadlineLatenessSeconds = 0.0;
	int32 QueueDepth = 0;
	double OldestCommandAgeSeconds = 0.0;
};
