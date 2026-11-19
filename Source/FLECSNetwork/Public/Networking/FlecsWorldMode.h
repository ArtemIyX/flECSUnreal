#pragma once

#include "Engine/EngineBaseTypes.h"

/** Standalone and both server modes own authoritative simulation state. */
inline bool FlecsHasAuthoritativeSimulation(ENetMode NetMode)
{
	return NetMode == NM_Standalone || NetMode == NM_ListenServer || NetMode == NM_DedicatedServer;
}
