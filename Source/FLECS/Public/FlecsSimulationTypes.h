#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsSimulationPublication.h"
#include "Data/Structs/FlecsSimulationDataItem.h"
#include "Data/Structs/FlecsSimulationDataPublication.h"
#include "Data/Structs/FlecsNamedWorkerDataPublisher.h"
#include "Data/Structs/FlecsNamedWorkerLifecycle.h"
#include "Data/Enums/FlecsDataPublicationAddResult.h"
#include "Data/Enums/FlecsWorkerCommandResult.h"
#include "FlecsSimulationCallbacks.h"
#include "../../ThirdParty/flecs/Include/flecs.h"

#include <functional>

/** DTO publication hard limits: 16,384 records (supports the 5,000-entity stress
 * case), 4 MiB total, and 64 KiB per record. */
inline constexpr uint32 FlecsMaxDataPublicationItems = 16384;
inline constexpr uint32 FlecsMaxDataPublicationBytes = 4 * 1024 * 1024;
inline constexpr uint32 FlecsMaxDataPublicationItemBytes = 64 * 1024;
inline constexpr uint32 FlecsMaxDataPublishers = 128;

/** Worker-only bounded writer. TryAdd copies bytes, so the resulting publication
 * never aliases Flecs storage or caller-owned buffers. */
class FFlecsSimulationDataBuilder
{
public:
	EFlecsDataPublicationAddResult TryAdd(uint32 TypeId, TArrayView<const uint8> Payload)
	{
		if (TypeId == 0)
		{
			++RejectedItems;
			return EFlecsDataPublicationAddResult::InvalidType;
		}
		if (Payload.Num() > 0 && Payload.GetData() == nullptr)
		{
			++RejectedItems;
			return EFlecsDataPublicationAddResult::InvalidPayload;
		}
		const uint32 PayloadBytes = static_cast<uint32>(Payload.Num());
		if (PayloadBytes > FlecsMaxDataPublicationItemBytes)
		{
			++RejectedItems;
			return EFlecsDataPublicationAddResult::ItemTooLarge;
		}
		if (Items.Num() >= static_cast<int32>(FlecsMaxDataPublicationItems))
		{
			++RejectedItems;
			return EFlecsDataPublicationAddResult::ItemLimitReached;
		}
		if (PayloadBytes > FlecsMaxDataPublicationBytes - TotalBytes)
		{
			++RejectedItems;
			return EFlecsDataPublicationAddResult::ByteLimitReached;
		}
		FFlecsSimulationDataItem& Item = Items.AddDefaulted_GetRef();
		Item.TypeId = TypeId;
		if (PayloadBytes > 0)
		{
			Item.Payload.Append(Payload.GetData(), Payload.Num());
		}
		TotalBytes += PayloadBytes;
		return EFlecsDataPublicationAddResult::Accepted;
	}

	uint32 GetTotalBytes() const { return TotalBytes; }
	uint32 GetRejectedItems() const { return RejectedItems; }
	const TArray<FFlecsSimulationDataItem>& GetItems() const { return Items; }
	TArray<FFlecsSimulationDataItem> TakeItems() { return MoveTemp(Items); }

private:
	TArray<FFlecsSimulationDataItem> Items;
	uint32 TotalBytes = 0;
	uint32 RejectedItems = 0;
};