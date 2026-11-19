#pragma once

#include "CoreMinimal.h"
#include "FlecsSimulationCallbacks.h"

struct FFlecsNamedWorkerDataPublisher
{
	FName Name;
	FFlecsWorkerDataPublisher Callback;
};
