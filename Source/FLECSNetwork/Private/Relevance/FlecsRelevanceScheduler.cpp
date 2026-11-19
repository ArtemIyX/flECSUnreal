#include "Relevance/FlecsRelevanceScheduler.h"

namespace
{
constexpr float MinCellSize = 1.0f;
constexpr float MaxInterestDistance = 1000000.0f;
constexpr uint32 ExitRecordBytes = 45; // Match the channel's per-record framing budget.
constexpr double MaxAgePrioritySeconds = 3600.0;

uint64 GetFairnessKey(const FFlecsNetworkEntityKey& Key)
{
	return (static_cast<uint64>(GetTypeHash(Key)) << 32) | GetTypeHash(Key.WorldEpoch ^ Key.EntityId);
}
}

FIntVector FFlecsSpatialIndex::ToCell(const FVector3f& Position, float CellSize)
{
	return FIntVector(FMath::FloorToInt(Position.X / CellSize), FMath::FloorToInt(Position.Y / CellSize),
		FMath::FloorToInt(Position.Z / CellSize));
}

uint64 FFlecsSpatialIndex::GetTrackedContainerBytes() const
{
	return static_cast<uint64>(Cells.GetAllocatedSize()) + CellArrayBytes;
}

bool FFlecsSpatialIndex::Build(const FFlecsSpatialSnapshot& InSnapshot, float CellSize)
{
	Snapshot = nullptr;
	Cells.Reset();
	CellArrayBytes = 0;
	GridCellSize = 0.0f;
	if (InSnapshot.WorldEpoch == 0 || InSnapshot.Entities.Num() > MaxEntities || !FMath::IsFinite(CellSize)
		|| CellSize < MinCellSize)
	{
		return false;
	}

	TSet<uint32> SeenEntityIds;
	SeenEntityIds.Reserve(InSnapshot.Entities.Num());
	for (int32 Index = 0; Index < InSnapshot.Entities.Num(); ++Index)
	{
		const FFlecsSpatialEntity& Entity = InSnapshot.Entities[Index];
		if (!Entity.Key.IsValid() || Entity.Key.WorldEpoch != InSnapshot.WorldEpoch
			|| !FMath::IsFinite(Entity.RecommendedUpdateRateHz) || Entity.RecommendedUpdateRateHz < 0.0f
			|| Entity.RecommendedUpdateRateHz > 60.0f || !FMath::IsFinite(Entity.Position.X)
			|| !FMath::IsFinite(Entity.Position.Y) || !FMath::IsFinite(Entity.Position.Z)
			|| FMath::Abs(Entity.Position.X) > MaxInterestDistance * 10.0f
			|| FMath::Abs(Entity.Position.Y) > MaxInterestDistance * 10.0f
			|| FMath::Abs(Entity.Position.Z) > MaxInterestDistance * 10.0f
			|| SeenEntityIds.Contains(Entity.Key.EntityId))
		{
			Cells.Reset();
			CellArrayBytes = 0;
			return false;
		}
		SeenEntityIds.Add(Entity.Key.EntityId);
		TArray<int32>& Cell = Cells.FindOrAdd(ToCell(Entity.Position, CellSize));
		const uint64 PreviousBytes = Cell.GetAllocatedSize();
		Cell.Add(Index);
		CellArrayBytes += static_cast<uint64>(Cell.GetAllocatedSize()) - PreviousBytes;
	}

	Snapshot = &InSnapshot;
	GridCellSize = CellSize;
	return true;
}

void FFlecsSpatialIndex::QuerySphere(
	const FVector3f& Center, float Radius, TArray<const FFlecsSpatialEntity*>& OutEntities) const
{
	OutEntities.Reset();
	if (!Snapshot || !FMath::IsFinite(Radius) || Radius < 0.0f || Radius > MaxInterestDistance * 2.0f
		|| !FMath::IsFinite(Center.X) || !FMath::IsFinite(Center.Y) || !FMath::IsFinite(Center.Z)
		|| FMath::Abs(Center.X) > MaxInterestDistance * 10.0f || FMath::Abs(Center.Y) > MaxInterestDistance * 10.0f
		|| FMath::Abs(Center.Z) > MaxInterestDistance * 10.0f)
	{
		return;
	}

	const FIntVector Min = ToCell(Center - FVector3f(Radius), GridCellSize);
	const FIntVector Max = ToCell(Center + FVector3f(Radius), GridCellSize);
	const int64 CellCount = (static_cast<int64>(Max.X) - Min.X + 1) * (static_cast<int64>(Max.Y) - Min.Y + 1)
						  * (static_cast<int64>(Max.Z) - Min.Z + 1);
	const float RadiusSquared = Radius * Radius;
	if (CellCount <= 0 || CellCount > MaxCellsPerQuery)
	{
		for (const FFlecsSpatialEntity& Entity : Snapshot->Entities)
		{
			if (FVector3f::DistSquared(Entity.Position, Center) <= RadiusSquared)
			{
				OutEntities.Add(&Entity);
			}
		}
		return;
	}

	for (int32 X = Min.X; X <= Max.X; ++X)
	{
		for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
		{
			for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
			{
				if (const TArray<int32>* Indices = Cells.Find(FIntVector(X, Y, Z)))
				{
					for (const int32 Index : *Indices)
					{
						const FFlecsSpatialEntity& Entity = Snapshot->Entities[Index];
						if (FVector3f::DistSquared(Entity.Position, Center) <= RadiusSquared)
						{
							OutEntities.Add(&Entity);
						}
					}
				}
			}
		}
	}
}

TArray<FFlecsRelevanceTier> FFlecsPerConnectionScheduler::MakeDefaultTiers()
{
	return {{5000.0f, 30.0f}, {15000.0f, 15.0f}, {30000.0f, 7.5f}, {60000.0f, 3.75f}};
}

bool FFlecsPerConnectionScheduler::IsRelevant(const FFlecsSpatialEntity& Entity,
	const FFlecsConnectionInterest& Interest, const FTrackedState* Previous, uint8& OutTier) const
{
	OutTier = 0;
	if (Entity.Visibility == EFlecsNetworkVisibility::OwnerOnly
		&& (Entity.OwnerConnectionId == 0 || Entity.OwnerConnectionId != Interest.ConnectionId))
	{
		return false;
	}
	const float DistanceSquared = FVector3f::DistSquared(Entity.Position, Interest.ViewPosition);
	auto SelectDistanceTier = [&Interest, DistanceSquared, &OutTier]() {
		OutTier = static_cast<uint8>(Interest.Tiers.Num() - 1);
		for (int32 TierIndex = 0; TierIndex < Interest.Tiers.Num(); ++TierIndex)
		{
			if (DistanceSquared <= FMath::Square(Interest.Tiers[TierIndex].MaxDistance))
			{
				OutTier = static_cast<uint8>(TierIndex);
				break;
			}
		}
	};

	if (const bool* bOverride = Interest.VisibilityOverrides.Find(Entity.Key))
	{
		if (!*bOverride)
		{
			return false;
		}
		SelectDistanceTier();
		return true;
	}
	if (Entity.bAlwaysRelevant)
	{
		SelectDistanceTier();
		return true;
	}

	for (int32 TierIndex = 0; TierIndex < Interest.Tiers.Num(); ++TierIndex)
	{
		const float TierRadius = Interest.Tiers[TierIndex].MaxDistance;
		if (DistanceSquared <= TierRadius * TierRadius)
		{
			OutTier = static_cast<uint8>(TierIndex);
			return true;
		}
	}
	if (Previous && Previous->bRelevant && Interest.Tiers.IsValidIndex(Previous->Tier))
	{
		const float RetainedRadius = Interest.Tiers[Previous->Tier].MaxDistance + Interest.HysteresisDistance;
		if (DistanceSquared <= RetainedRadius * RetainedRadius)
		{
			OutTier = Previous->Tier;
			return true;
		}
	}
	return false;
}

double FFlecsPerConnectionScheduler::GetTierPeriod(
	const FFlecsConnectionInterest& Interest, uint8 Tier, float EntityRateCapHz)
{
	float Rate = Interest.Tiers.IsValidIndex(Tier) ? Interest.Tiers[Tier].UpdateRateHz : 0.0f;
	if (FMath::IsFinite(EntityRateCapHz) && EntityRateCapHz > 0.0f)
	{
		Rate = Rate > 0.0f ? FMath::Min(Rate, EntityRateCapHz) : EntityRateCapHz;
	}
	return FMath::IsFinite(Rate) && Rate > 0.0f ? 1.0 / static_cast<double>(Rate) : 0.0;
}

bool FFlecsPerConnectionScheduler::Select(const FFlecsSpatialIndex& Index, const FFlecsConnectionInterest& Interest,
	double NowSeconds, const FFlecsRelevanceBudget& Budget, FFlecsRelevanceSelection& OutSelection,
	uint32 MaxRelevantEntities, const TMap<FFlecsNetworkEntityKey, double>* BlockedUntil)
{
	OutSelection = FFlecsRelevanceSelection();
	if (OutstandingSelectionId != 0)
	{
		return false;
	}
	const FFlecsSpatialSnapshot* Snapshot = Index.GetSnapshot();
	if (!Snapshot || Interest.ConnectionId == 0 || !FMath::IsFinite(NowSeconds) || Interest.Tiers.IsEmpty()
		|| Interest.Tiers.Num() > static_cast<int32>(MaxTiers) || !FMath::IsFinite(Interest.HysteresisDistance)
		|| Interest.HysteresisDistance < 0.0f || Interest.HysteresisDistance > MaxInterestDistance
		|| Snapshot->Entities.Num() > MaxTrackedEntities || !FMath::IsFinite(Interest.ViewPosition.X)
		|| !FMath::IsFinite(Interest.ViewPosition.Y) || !FMath::IsFinite(Interest.ViewPosition.Z)
		|| FMath::Abs(Interest.ViewPosition.X) > MaxInterestDistance * 10.0f
		|| FMath::Abs(Interest.ViewPosition.Y) > MaxInterestDistance * 10.0f
		|| FMath::Abs(Interest.ViewPosition.Z) > MaxInterestDistance * 10.0f
		|| Interest.VisibilityOverrides.Num() > MaxTrackedEntities || Budget.MaxRecords == 0 || Budget.MaxBytes == 0
		|| MaxRelevantEntities == 0 || MaxRelevantEntities > static_cast<uint32>(MaxTrackedEntities))
	{
		return false;
	}
	float PreviousRadius = 0.0f;
	for (const FFlecsRelevanceTier& Tier : Interest.Tiers)
	{
		if (!FMath::IsFinite(Tier.MaxDistance) || Tier.MaxDistance <= PreviousRadius
			|| Tier.MaxDistance > MaxInterestDistance || !FMath::IsFinite(Tier.UpdateRateHz)
			|| Tier.UpdateRateHz <= 0.0f || Tier.UpdateRateHz > 60.0f)
		{
			return false;
		}
		PreviousRadius = Tier.MaxDistance;
	}
	if (ActiveEpoch != 0 && Snapshot->WorldEpoch < ActiveEpoch)
	{
		return false;
	}
	const bool bNewEpoch = ActiveEpoch == 0 || Snapshot->WorldEpoch > ActiveEpoch;
	if (!bNewEpoch && ActiveConnectionId != 0 && ActiveConnectionId != Interest.ConnectionId)
	{
		return false;
	}
	if (Tracked.Num() > MaxTrackedEntities)
	{
		return false;
	}

	TArray<FFlecsNetworkEntityKey> DerivedLiveKeys;
	const TArray<FFlecsNetworkEntityKey>* SnapshotLiveKeys = &Snapshot->LiveEntityKeys;
	if (Snapshot->bHasCompleteLiveEntitySet)
	{
		if (Snapshot->LiveEntityKeys.Num() > FFlecsSpatialIndex::MaxEntities)
		{
			return false;
		}
	}
	else
	{
		DerivedLiveKeys.Reserve(Snapshot->Entities.Num());
		for (const FFlecsSpatialEntity& Entity : Snapshot->Entities)
		{
			DerivedLiveKeys.Add(Entity.Key);
		}
		SnapshotLiveKeys = &DerivedLiveKeys;
	}

	TMap<uint32, uint16> SnapshotGenerations;
	SnapshotGenerations.Reserve(SnapshotLiveKeys->Num());
	int32 NewHistoryEntries = 0;
	const int32 ExistingHistoryEntries = bNewEpoch ? 0 : HighestGenerationByEntityId.Num();
	for (const FFlecsNetworkEntityKey& Key : *SnapshotLiveKeys)
	{
		if (!Key.IsValid() || Key.WorldEpoch != Snapshot->WorldEpoch || SnapshotGenerations.Contains(Key.EntityId))
		{
			return false;
		}
		SnapshotGenerations.Add(Key.EntityId, Key.Generation);
		const uint16* HighestGeneration = bNewEpoch ? nullptr : HighestGenerationByEntityId.Find(Key.EntityId);
		if (!HighestGeneration && ++NewHistoryEntries > IdentityHistoryLimit - ExistingHistoryEntries)
		{
			OutSelection.Metrics.bIdentityHistoryExhausted = true;
			UE_LOG(LogTemp, Error, TEXT("Flecs relevance identity history limit reached; rejecting snapshot."));
			return false;
		}
		if (!bNewEpoch)
		{
			if ((HighestGeneration && Key.Generation < *HighestGeneration)
				|| (HighestGeneration && Key.Generation == *HighestGeneration
					&& RetiredEntityIds.Contains(Key.EntityId)))
			{
				return false;
			}
		}
	}
	if (Snapshot->bHasCompleteLiveEntitySet)
	{
		for (const FFlecsSpatialEntity& Entity : Snapshot->Entities)
		{
			const uint16* LiveGeneration = SnapshotGenerations.Find(Entity.Key.EntityId);
			if (!LiveGeneration || *LiveGeneration != Entity.Key.Generation
				|| Entity.Key.WorldEpoch != Snapshot->WorldEpoch)
			{
				return false;
			}
		}
	}

	if (bNewEpoch)
	{
		Tracked.Reset();
		HighestGenerationByEntityId.Reset();
		RetiredEntityIds.Reset();
		ActiveEpoch = Snapshot->WorldEpoch;
		ActiveConnectionId = 0;
	}
	ActiveConnectionId = Interest.ConnectionId;
	for (const TPair<uint32, uint16>& Pair : SnapshotGenerations)
	{
		uint16* HighestGeneration = HighestGenerationByEntityId.Find(Pair.Key);
		if (!HighestGeneration || Pair.Value > *HighestGeneration)
		{
			HighestGenerationByEntityId.Add(Pair.Key, Pair.Value);
			RetiredEntityIds.Remove(Pair.Key);
		}
	}
	// A snapshot is a complete worker publication. Absence retires the current
	// generation immediately, even if a network exit record is still budget-deferred.
	for (const TPair<uint32, uint16>& Pair : HighestGenerationByEntityId)
	{
		if (!SnapshotGenerations.Contains(Pair.Key))
		{
			RetiredEntityIds.Add(Pair.Key);
		}
	}

	TArray<const FFlecsSpatialEntity*> Nearby;
	Index.QuerySphere(Interest.ViewPosition, Interest.Tiers.Last().MaxDistance + Interest.HysteresisDistance, Nearby);
	TSet<FFlecsNetworkEntityKey> NearbyKeys;
	NearbyKeys.Reserve(Nearby.Num());
	for (const FFlecsSpatialEntity* Entity : Nearby)
	{
		if (Entity)
			NearbyKeys.Add(Entity->Key);
	}
	for (const FFlecsSpatialEntity& Entity : Snapshot->Entities)
	{
		const bool* bOverride = Interest.VisibilityOverrides.Find(Entity.Key);
		if (((Entity.bAlwaysRelevant || (bOverride && *bOverride)) || Tracked.Contains(Entity.Key))
			&& !NearbyKeys.Contains(Entity.Key))
		{
			Nearby.Add(&Entity);
			NearbyKeys.Add(Entity.Key);
		}
	}
	TArray<FCandidate> Candidates;
	Candidates.Reserve(Nearby.Num() + Tracked.Num());
	TSet<uint32> PendingPreviousGenerations;
	for (const TPair<FFlecsNetworkEntityKey, FTrackedState>& Pair : Tracked)
	{
		const uint16* LiveGeneration = SnapshotGenerations.Find(Pair.Key.EntityId);
		if (Pair.Value.bRelevant && LiveGeneration && *LiveGeneration != Pair.Key.Generation)
		{
			PendingPreviousGenerations.Add(Pair.Key.EntityId);
		}
	}
	for (const FFlecsSpatialEntity* Entity : Nearby)
	{
		if (!Entity || Entity->Key.WorldEpoch != Snapshot->WorldEpoch)
		{
			++OutSelection.Metrics.StaleSnapshotRecords;
			continue;
		}
		if (PendingPreviousGenerations.Contains(Entity->Key.EntityId))
		{
			// A single transfer cannot remove one generation and create another
			// for the same ID. Wait for the old exit's application ACK.
			continue;
		}
		FTrackedState* Previous = Tracked.Find(Entity->Key);
		uint8 Tier = 0;
		const bool bNowRelevant = IsRelevant(*Entity, Interest, Previous, Tier);
		if (!bNowRelevant)
		{
			if (Previous && Previous->bRelevant)
			{
				FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				Candidate.Record = {Entity->Key, EFlecsRelevanceTransition::Exit, Previous->Revision,
					Snapshot->SimulationTick, ExitRecordBytes, Previous->OwnerConnectionId, 0};
				Candidate.Importance = 1001;
				Candidate.DeferredSinceSeconds = Previous->LastSentAtSeconds;
				Candidate.FairnessOrder = GetFairnessKey(Entity->Key);
			}
			continue;
		}

		if (!Previous)
		{
			if (Tracked.Num() >= MaxTrackedEntities)
			{
				++OutSelection.Metrics.StaleSnapshotRecords;
				continue;
			}
			FTrackedState NewState;
			NewState.LastSentAtSeconds = NowSeconds;
			NewState.EnteredAtSeconds = NowSeconds;
			Previous = &Tracked.Add(Entity->Key, MoveTemp(NewState));
		}
		const bool bOwnershipChanged = Previous->bRelevant && Previous->OwnerConnectionId != Entity->OwnerConnectionId;
		const bool bEntering = !Previous->bRelevant || bOwnershipChanged;
		const bool bChanged = Previous->Revision != Entity->Revision;
		const bool bDue =
			NowSeconds - Previous->LastSentAtSeconds >= GetTierPeriod(Interest, Tier, Entity->RecommendedUpdateRateHz);
		if (bEntering || (bChanged && bDue))
		{
			const double* RetryAt = BlockedUntil ? BlockedUntil->Find(Entity->Key) : nullptr;
			if (RetryAt && NowSeconds < *RetryAt)
			{
				++OutSelection.Metrics.DeferredByCapacity;
				continue;
			}
			FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Record = {Entity->Key,
				bEntering ? EFlecsRelevanceTransition::Enter : EFlecsRelevanceTransition::Update, Entity->Revision,
				Snapshot->SimulationTick, Entity->EstimatedUpdateBytes, Entity->OwnerConnectionId, Tier};
			Candidate.Importance = FMath::Clamp(Entity->Importance, -1000, 1000);
			Candidate.DeferredSinceSeconds =
				Previous->bRelevant ? Previous->LastSentAtSeconds : Previous->EnteredAtSeconds;
			Candidate.FairnessOrder = GetFairnessKey(Entity->Key);
		}
		++OutSelection.Metrics.CurrentlyRelevant;
	}

	for (auto It = Tracked.CreateIterator(); It; ++It)
	{
		const uint16* LiveGeneration = SnapshotGenerations.Find(It.Key().EntityId);
		if (It.Key().WorldEpoch == Snapshot->WorldEpoch && LiveGeneration && *LiveGeneration == It.Key().Generation)
		{
			continue;
		}
		if (!It.Value().bRelevant)
		{
			It.RemoveCurrent();
			continue;
		}
		if (It.Value().bRelevant)
		{
			FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Record = {It.Key(), EFlecsRelevanceTransition::Exit, It.Value().Revision,
				Snapshot->SimulationTick, ExitRecordBytes, It.Value().OwnerConnectionId, 0};
			Candidate.Record.bEntityDespawned = true;
			Candidate.Importance = 1001;
			Candidate.DeferredSinceSeconds = It.Value().LastSentAtSeconds;
			Candidate.FairnessOrder = GetFairnessKey(It.Key());
		}
	}

	OutSelection.Metrics.CandidateCount = Candidates.Num();
	const uint64 FairnessStart = FairnessCursor;
	Candidates.Sort([NowSeconds, FairnessStart](const FCandidate& A, const FCandidate& B) {
		const double AAge = FMath::Clamp(NowSeconds - A.DeferredSinceSeconds, 0.0, MaxAgePrioritySeconds);
		const double BAge = FMath::Clamp(NowSeconds - B.DeferredSinceSeconds, 0.0, MaxAgePrioritySeconds);
		const double AScore = static_cast<double>(A.Importance) + AAge;
		const double BScore = static_cast<double>(B.Importance) + BAge;
		if (!FMath::IsNearlyEqual(AScore, BScore))
			return AScore > BScore;
		if (!FMath::IsNearlyEqual(AAge, BAge))
			return AAge > BAge;
		return (A.FairnessOrder - FairnessStart) < (B.FairnessOrder - FairnessStart);
	});

	uint32 RemainingBytes = Budget.MaxBytes;
	uint32 ReservedEntities = 0;
	for (const TPair<FFlecsNetworkEntityKey, FTrackedState>& Pair : Tracked)
	{
		if (Pair.Value.bRelevant)
			++ReservedEntities;
	}
	for (const FCandidate& Candidate : Candidates)
	{
		const FTrackedState* ExistingState = Tracked.Find(Candidate.Record.Key);
		const bool bNewAdmission = Candidate.Record.Transition == EFlecsRelevanceTransition::Enter
			&& (!ExistingState || !ExistingState->bRelevant);
		if (bNewAdmission && ReservedEntities >= MaxRelevantEntities)
		{
			++OutSelection.Metrics.DeferredByCapacity;
			continue;
		}
		const uint32 RecordBytes = Candidate.Record.Transition == EFlecsRelevanceTransition::Exit
									 ? ExitRecordBytes
									 : Candidate.Record.EstimatedBytes;
		if (RecordBytes > Budget.MaxBytes)
		{
			++OutSelection.Metrics.OversizedEntityCount;
			if (!OutSelection.Metrics.FirstOversizedEntity.IsValid())
			{
				OutSelection.Metrics.FirstOversizedEntity = Candidate.Record.Key;
			}
		}
		if (OutSelection.Records.Num() >= static_cast<int32>(Budget.MaxRecords) || RecordBytes > RemainingBytes)
		{
			++OutSelection.Metrics.DeferredByBudget;
			const uint64 AgeMs =
				static_cast<uint64>(FMath::Max(0.0, (NowSeconds - Candidate.DeferredSinceSeconds) * 1000.0));
			OutSelection.Metrics.OldestDeferredAgeMs = FMath::Max(OutSelection.Metrics.OldestDeferredAgeMs, AgeMs);
			continue;
		}
		OutSelection.Records.Add(Candidate.Record);
		if (bNewAdmission)
			++ReservedEntities;
		RemainingBytes -= RecordBytes;
		OutSelection.Metrics.SelectedBytes += RecordBytes;
	}
	OutSelection.Metrics.SelectedRecords = OutSelection.Records.Num();
	if (!Candidates.IsEmpty())
	{
		FairnessCursor = Candidates.Last().FairnessOrder + 1;
	}
	if (!OutSelection.Records.IsEmpty())
	{
		OutSelection.SelectionId = NextSelectionId++;
		if (NextSelectionId == 0)
			NextSelectionId = 1;
		OutstandingSelectionId = OutSelection.SelectionId;
		OutstandingRecords = OutSelection.Records;
	}
	return true;
}

bool FFlecsPerConnectionScheduler::CompleteSelection(
	uint64 SelectionId, bool bHandedToTransport, double HandedAtSeconds)
{
	if (SelectionId == 0 || SelectionId != OutstandingSelectionId
		|| (bHandedToTransport && !FMath::IsFinite(HandedAtSeconds)))
	{
		return false;
	}
	if (bHandedToTransport)
	{
		for (const FFlecsScheduledEntity& Record : OutstandingRecords)
		{
			if (Record.Transition == EFlecsRelevanceTransition::Exit)
			{
				Tracked.Remove(Record.Key);
				const uint16* HighestGeneration = HighestGenerationByEntityId.Find(Record.Key.EntityId);
				if (Record.bEntityDespawned && HighestGeneration && *HighestGeneration == Record.Key.Generation)
				{
					RetiredEntityIds.Add(Record.Key.EntityId);
				}
				continue;
			}
			if (FTrackedState* State = Tracked.Find(Record.Key))
			{
				State->bRelevant = true;
				State->Revision = Record.Revision;
				State->OwnerConnectionId = Record.OwnerConnectionId;
				State->LastSentAtSeconds = HandedAtSeconds;
				State->Tier = Record.Tier;
				if (Record.Transition == EFlecsRelevanceTransition::Enter)
				{
					State->EnteredAtSeconds = HandedAtSeconds;
				}
			}
		}
	}
	OutstandingRecords.Reset();
	OutstandingSelectionId = 0;
	return true;
}

bool FFlecsPerConnectionScheduler::Reset(uint64 NewWorldEpoch)
{
	if (NewWorldEpoch != 0 && ActiveEpoch != 0 && NewWorldEpoch < ActiveEpoch)
	{
		return false;
	}
	const bool bAdvanceEpoch = NewWorldEpoch != 0 && NewWorldEpoch > ActiveEpoch;
	Tracked.Reset();
	OutstandingRecords.Reset();
	OutstandingSelectionId = 0;
	if (bAdvanceEpoch)
	{
		ActiveEpoch = NewWorldEpoch;
		HighestGenerationByEntityId.Reset();
		RetiredEntityIds.Reset();
	}
	else if (ActiveEpoch == 0 && NewWorldEpoch != 0)
	{
		ActiveEpoch = NewWorldEpoch;
	}
	ActiveConnectionId = 0;
	FairnessCursor = 0;
	return true;
}
