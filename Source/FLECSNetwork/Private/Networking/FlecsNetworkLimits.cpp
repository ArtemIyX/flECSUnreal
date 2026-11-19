#include "Networking/FlecsNetworkLimits.h"

#include "Settings/FlecsDeveloperSettings.h"
#include "Simulation/FlecsWorkerEntityModel.h"

bool FFlecsNetworkLimits::Capture(const UFlecsDeveloperSettings& Settings, FFlecsNetworkLimits& OutLimits)
{
	OutLimits.MaxEntities = FMath::Clamp(Settings.MaxNetworkEntities, 1, FlecsSimulation::MaxEntities);
	OutLimits.MaxOwnedEntitiesPerPeer = FMath::Clamp(Settings.MaxPeerOwnedEntities, 1, OutLimits.MaxEntities);
	OutLimits.MaxRelevantEntitiesPerPeer = FMath::Clamp(Settings.MaxRelevantEntitiesPerPeer, 1, OutLimits.MaxEntities);
	OutLimits.MaxRecordsPerSelection = static_cast<uint32>(
		FMath::Clamp(Settings.MaxRecordsPerPeerSelection, 1, FlecsSimulation::MaxRecordsPerBatch));
	OutLimits.MaxPayloadBytesPerSelection = static_cast<uint32>(
		FMath::Clamp(Settings.MaxPayloadKiBPerPeerSelection, 1, static_cast<int32>(FlecsSimulation::MaxBatchBytes / 1024))) * 1024u;
	return OutLimits.MaxEntities == Settings.MaxNetworkEntities
		&& OutLimits.MaxOwnedEntitiesPerPeer == Settings.MaxPeerOwnedEntities
		&& OutLimits.MaxRelevantEntitiesPerPeer == Settings.MaxRelevantEntitiesPerPeer
		&& static_cast<int32>(OutLimits.MaxRecordsPerSelection) == Settings.MaxRecordsPerPeerSelection
		&& static_cast<int32>(OutLimits.MaxPayloadBytesPerSelection / 1024u) == Settings.MaxPayloadKiBPerPeerSelection;
}
