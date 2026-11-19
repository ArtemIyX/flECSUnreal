#pragma once

#include "CoreMinimal.h"
#include <functional>

namespace flecs { struct world; }
class FFlecsSimulationDataBuilder;

using FFlecsWorkerCommand = std::function<void(flecs::world&)>;
/** Registered before worker startup; invoked on the simulation thread after each
 * fixed step. Must not access UObjects or store Flecs references in the DTO. */
using FFlecsWorkerDataPublisher = std::function<void(flecs::world&, FFlecsSimulationDataBuilder&)>;
/** Paired worker lifecycle callbacks. Both execute on the Flecs owner thread and
 * receive only that session's world and local Unreal attachment epoch. They must
 * not capture or access UObjects, actors, or Unreal connection state. */
using FFlecsWorkerStartupCallback = std::function<void(flecs::world&, uint64 LocalAttachmentEpoch)>;
using FFlecsWorkerShutdownCallback = std::function<void(flecs::world&, uint64 LocalAttachmentEpoch)>;
