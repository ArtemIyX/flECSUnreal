#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"

namespace FlecsSimulation
{
/**
	 * Trusted per-peer selection supplied by the network bridge after scheduling.
	 * Entity keys are packed from network ID and generation with MakeEntityViewKey.
	 * The context is value-only and must be built for one live connection incarnation.
	 */
	struct FPeerReplicationContext
	{
		uint64 ConnectionToken = 0;
		TSet<uint64> RelevantEntityKeys;

		static uint64 MakeEntityViewKey(uint32 NetworkEntityId, uint16 Generation)
		{
			return (static_cast<uint64>(NetworkEntityId) << 16) | Generation;
		}

		bool Includes(const FNetworkEntityIdentity& Identity) const
		{
			return Identity.IsValid() && RelevantEntityKeys.Contains(
				MakeEntityViewKey(Identity.NetworkEntityId, Identity.Generation));
		}
	};
}
