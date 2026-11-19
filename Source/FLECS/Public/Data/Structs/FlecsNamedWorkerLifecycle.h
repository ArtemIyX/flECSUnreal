#pragma once

#include "CoreMinimal.h"
#include "FlecsSimulationCallbacks.h"

struct FFlecsNamedWorkerLifecycle
{
	FName Name;
	FFlecsWorkerStartupCallback Startup;
	FFlecsWorkerShutdownCallback Shutdown;
};
