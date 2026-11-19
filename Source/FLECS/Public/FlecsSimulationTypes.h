#pragma once

#include "CoreMinimal.h"
#include "../../ThirdParty/flecs/Include/flecs.h"

#include <functional>

/** DTO publication hard limits: 16,384 records (supports the 5,000-entity stress
 * case), 4 MiB total, and 64 KiB per record. */
inline constexpr uint32 FlecsMaxDataPublicationItems = 16384;
inline constexpr uint32 FlecsMaxDataPublicationBytes = 4 * 1024 * 1024;
inline constexpr uint32 FlecsMaxDataPublicationItemBytes = 64 * 1024;
inline constexpr uint32 FlecsMaxDataPublishers = 128;

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

using FFlecsWorkerCommand = std::function<void(flecs::world&)>;

struct FFlecsSimulationDataItem
{
	/** Nonzero application-defined schema/type identifier. */
	uint32 TypeId = 0;
	/** Owned copy of the encoded application DTO. */
	TArray<uint8> Payload;
};

/** Immutable snapshot data contains copied application bytes only. Epoch identifies
 * the local Unreal world attachment; it is not a server or network-session epoch. */
struct FFlecsSimulationDataPublication
{
	uint64 LocalAttachmentEpoch = 0;
	uint64 SimulationTick = 0;
	/** Monotonic platform seconds sampled after extraction completes. */
	double TimestampSeconds = 0.0;
	uint32 TotalBytes = 0;
	uint32 RejectedItems = 0;
	TArray<FFlecsSimulationDataItem> Items;
};

enum class EFlecsDataPublicationAddResult : uint8
{
	Accepted,
	InvalidType,
	InvalidPayload,
	ItemTooLarge,
	ItemLimitReached,
	ByteLimitReached
};

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

/** Registered before worker startup; invoked on the simulation thread after each
 * fixed step. Must not access UObjects or store Flecs references in the DTO. */
using FFlecsWorkerDataPublisher = std::function<void(flecs::world&, FFlecsSimulationDataBuilder&)>;

enum class EFlecsWorkerCommandResult : uint8
{
	Accepted,
	InvalidCommand,
	StaleEpoch,
	QueueFull,
	Stopping
};
