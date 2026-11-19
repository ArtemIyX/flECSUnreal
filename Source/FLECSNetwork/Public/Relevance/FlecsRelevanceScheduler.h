#pragma once

#include "CoreMinimal.h"
#include "Data/Structs/FlecsNetworkEntityKey.h"
#include "Data/Structs/FlecsSpatialEntity.h"
#include "Data/Structs/FlecsSpatialSnapshot.h"
#include "Data/Structs/FlecsRelevanceTier.h"
#include "Data/Structs/FlecsConnectionInterest.h"
#include "Data/Structs/FlecsRelevanceBudget.h"
#include "Data/Structs/FlecsScheduledEntity.h"
#include "Data/Structs/FlecsRelevanceMetrics.h"
#include "Data/Structs/FlecsRelevanceSelection.h"
#include "Data/Enums/FlecsNetworkVisibility.h"
#include "Data/Enums/FlecsRelevanceTransition.h"

/** Bounded uniform-grid index over copied publication data. Rebuild once per snapshot. */
class FLECSNETWORK_API FFlecsSpatialIndex
{
public:
	static constexpr int32 MaxEntities = 5000;
	static constexpr int32 MaxCellsPerQuery = 32768;

	bool Build(const FFlecsSpatialSnapshot& Snapshot, float CellSize = 2000.0f);
	void QuerySphere(const FVector3f& Center, float Radius, TArray<const FFlecsSpatialEntity*>& OutEntities) const;
	const FFlecsSpatialSnapshot* GetSnapshot() const { return Snapshot; }

private:
	static FIntVector ToCell(const FVector3f& Position, float CellSize);

	const FFlecsSpatialSnapshot* Snapshot = nullptr;
	float GridCellSize = 0.0f;
	TMap<FIntVector, TArray<int32>> Cells;
};

/**
 * Per-connection, bounded scheduler. Call only on the network/scheduling owner thread.
 * Select returns one outstanding work assignment. Call CompleteSelection after
 * the transport accepts it; a rejected handoff leaves the records due for retry.
 */
class FLECSNETWORK_API FFlecsPerConnectionScheduler
{
public:
	static constexpr int32 MaxTrackedEntities = 10000;
	static constexpr int32 MaxIdentityHistory = 100000;
	static constexpr uint32 MaxTiers = 4;

	static TArray<FFlecsRelevanceTier> MakeDefaultTiers();
	bool Select(const FFlecsSpatialIndex& Index, const FFlecsConnectionInterest& Interest, double NowSeconds,
		const FFlecsRelevanceBudget& Budget, FFlecsRelevanceSelection& OutSelection);
	bool CompleteSelection(uint64 SelectionId, bool bHandedToTransport, double HandedAtSeconds);
	/** Clear per-connection relevance state; epoch and generation history never move backwards. */
	bool Reset(uint64 NewWorldEpoch = 0);

private:
	struct FTrackedState
	{
		uint64 Revision = 0;
		uint64 OwnerConnectionId = 0;
		double EnteredAtSeconds = 0.0;
		double LastSentAtSeconds = 0.0;
		uint8 Tier = 0;
		bool bRelevant = false;
	};

	struct FCandidate
	{
		FFlecsScheduledEntity Record;
		int32 Importance = 0;
		double DeferredSinceSeconds = 0.0;
		uint64 FairnessOrder = 0;
	};

	bool IsRelevant(const FFlecsSpatialEntity& Entity, const FFlecsConnectionInterest& Interest,
		const FTrackedState* Previous, uint8& OutTier) const;
	static double GetTierPeriod(const FFlecsConnectionInterest& Interest, uint8 Tier, float EntityRateCapHz);

	TMap<FFlecsNetworkEntityKey, FTrackedState> Tracked;
	TMap<uint32, uint16> HighestGenerationByEntityId;
	TSet<uint32> RetiredEntityIds;
	TArray<FFlecsScheduledEntity> OutstandingRecords;
	uint64 ActiveEpoch = 0;
	uint64 ActiveConnectionId = 0;
	uint64 FairnessCursor = 0;
	uint64 NextSelectionId = 1;
	uint64 OutstandingSelectionId = 0;
};
