#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsSessionWireEntry.h"

namespace FlecsModRegistry
{
/** Immutable IDs for one peer, issued only by a successful exact comparison. */
	class FLECSMODDING_API FSessionMapping
	{
	public:
		bool IsValid() const { return bValidated; }
		bool FindWireId(EEntryKind Kind, const FString& StableId, uint16& OutId) const;
		bool FindEntryByWireId(uint16 WireId, FSessionWireEntry& OutEntry) const;

	private:
		friend class FRegistry;
		TMap<FString, uint16> WireIds;
		TMap<uint16, FSessionWireEntry> EntriesByWireId;
		bool bValidated = false;
	};
}
