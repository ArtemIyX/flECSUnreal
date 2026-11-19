#pragma once

#include "CoreMinimal.h"

/**
 * Network identity copied into immutable worker publications; never a local Flecs ID.
 * An entity ID is unique within one epoch; after despawn, reuse it only with a
 * strictly greater generation. Generation zero is reserved as invalid.
 */
struct FLECSNETWORK_API FFlecsNetworkEntityKey
{
	uint64 WorldEpoch = 0;
	uint32 EntityId = 0;
	uint16 Generation = 0;

	bool IsValid() const { return WorldEpoch != 0 && EntityId != 0 && Generation != 0; }
	friend bool operator==(const FFlecsNetworkEntityKey& A, const FFlecsNetworkEntityKey& B)
	{
		return A.WorldEpoch == B.WorldEpoch && A.EntityId == B.EntityId && A.Generation == B.Generation;
	}
	friend uint32 GetTypeHash(const FFlecsNetworkEntityKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.WorldEpoch), GetTypeHash(Key.EntityId)), GetTypeHash(Key.Generation));
	}
};

enum class EFlecsNetworkVisibility : uint8
{
	RelevantToAll,
	OwnerOnly
};

/** Complete spatial/revision metadata copied at one simulation publication boundary. */
struct FLECSNETWORK_API FFlecsSpatialEntity
{
	FFlecsNetworkEntityKey Key;
	FVector3f Position = FVector3f::ZeroVector;
	uint64 Revision = 0;
	uint64 OwnerConnectionId = 0;
	uint32 EstimatedUpdateBytes = 0;
	/** Optional schema-derived cap for the entity cadence; zero uses its distance tier unchanged. */
	float RecommendedUpdateRateHz = 0.0f;
	int32 Importance = 0;
	EFlecsNetworkVisibility Visibility = EFlecsNetworkVisibility::RelevantToAll;
	bool bAlwaysRelevant = false;
};

/**
 * One immutable, bounded simulation publication used by spatial selection.
 * The caller keeps this object alive and unchanged while its spatial index is used.
 */
struct FLECSNETWORK_API FFlecsSpatialSnapshot
{
	uint64 WorldEpoch = 0;
	uint64 SimulationTick = 0;
	TArray<FFlecsSpatialEntity> Entities;
	/** Complete worker-owned identity list, independent of partial spatial metadata. */
	TArray<FFlecsNetworkEntityKey> LiveEntityKeys;
	/** When true, only LiveEntityKeys may establish entity absence or generation retirement. */
	bool bHasCompleteLiveEntitySet = false;
};

struct FLECSNETWORK_API FFlecsRelevanceTier
{
	float MaxDistance = 0.0f;
	float UpdateRateHz = 0.0f;
};

struct FLECSNETWORK_API FFlecsConnectionInterest
{
	uint64 ConnectionId = 0;
	FVector3f ViewPosition = FVector3f::ZeroVector;
	/** Per-connection explicit show/hide overrides; absent keys use distance/visibility policy. */
	TMap<FFlecsNetworkEntityKey, bool> VisibilityOverrides;
	TArray<FFlecsRelevanceTier> Tiers;
	float HysteresisDistance = 250.0f;
};

struct FLECSNETWORK_API FFlecsRelevanceBudget
{
	uint32 MaxRecords = 128;
	uint32 MaxBytes = 64 * 1024;
};

enum class EFlecsRelevanceTransition : uint8
{
	Enter,
	Update,
	Exit
};

struct FLECSNETWORK_API FFlecsScheduledEntity
{
	FFlecsNetworkEntityKey Key;
	EFlecsRelevanceTransition Transition = EFlecsRelevanceTransition::Update;
	uint64 Revision = 0;
	uint64 SimulationTick = 0;
	uint32 EstimatedBytes = 0;
	uint64 OwnerConnectionId = 0;
	uint8 Tier = 0;
	bool bEntityDespawned = false;
};

struct FLECSNETWORK_API FFlecsRelevanceMetrics
{
	uint32 CandidateCount = 0;
	uint32 SelectedRecords = 0;
	uint32 SelectedBytes = 0;
	uint32 DeferredByBudget = 0;
	uint32 OversizedEntityCount = 0;
	FFlecsNetworkEntityKey FirstOversizedEntity;
	uint32 StaleSnapshotRecords = 0;
	uint32 CurrentlyRelevant = 0;
	uint64 OldestDeferredAgeMs = 0;
};

struct FLECSNETWORK_API FFlecsRelevanceSelection
{
	uint64 SelectionId = 0;
	TArray<FFlecsScheduledEntity> Records;
	FFlecsRelevanceMetrics Metrics;
};

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
	bool Select(const FFlecsSpatialIndex& Index, const FFlecsConnectionInterest& Interest,
		double NowSeconds, const FFlecsRelevanceBudget& Budget, FFlecsRelevanceSelection& OutSelection);
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
