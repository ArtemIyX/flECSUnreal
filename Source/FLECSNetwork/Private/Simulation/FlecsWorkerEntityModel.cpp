#include "Simulation/FlecsWorkerEntityModel.h"

#include "FlecsSimulationTypes.h"
#include "HAL/PlatformTLS.h"

namespace FlecsSimulation
{
	namespace
	{
		constexpr uint32 MaxPayloadBytesPerRecord = 64 * 1024;
		// FFlecsReplicationRecord's serialized state fields, excluding Fast Array framing.
		constexpr uint32 EstimatedReplicationRecordHeaderBytes = 45; // Match the channel's framing budget.

		bool IsValidOperation(EStateOperation Operation)
		{
			return static_cast<uint8>(Operation) <= static_cast<uint8>(EStateOperation::EntityLeaveRelevance);
		}

		bool IsValidVisibility(FlecsModRegistry::EVisibilityPolicy Visibility)
		{
			return static_cast<uint8>(Visibility) <= static_cast<uint8>(FlecsModRegistry::EVisibilityPolicy::Always);
		}
	}

	FWorkerEntityModel::FWorkerEntityModel(FlecsModRegistry::FRegistry& InRegistry, bool bInAuthoritative)
		: Registry(InRegistry)
		  , bAuthoritative(bInAuthoritative)
	{
	}

	FWorkerEntityModel::~FWorkerEntityModel()
	{
		// The runner destroys its Flecs world on the owning thread; destruction of
		// this value object must not touch Flecs handles from another thread.
	}

	EModelResult FWorkerEntityModel::Initialize(flecs::world& InWorkerWorld, uint64 InServerWorldEpoch,
	                                            FPermissionCheck InPermissionCheck)
	{
		if (bInitialized || InServerWorldEpoch == 0 || !Registry.IsFrozen() ||
			(bAuthoritative && !InPermissionCheck))
		{
			return EModelResult::Invalid;
		}
		WorkerWorld = &InWorkerWorld;
		ServerWorldEpoch = InServerWorldEpoch;
		PermissionCheck = MoveTemp(InPermissionCheck);
		OwnerThreadId = FPlatformTLS::GetCurrentThreadId();
		bAuthoritativeSessionUsable = true;
		bInitialized = true;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::Shutdown()
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		TArray<uint32> EntityIds;
		EntitiesById.GenerateKeyArray(EntityIds);
		for (uint32 EntityId : EntityIds) RemoveEntity(EntityId);
		ReplicaHistoryById.Empty();
		DespawnTombstonesById.Empty();
		CommandWindows.Empty();
		PeerDeltaStates.Empty();
		SpatialDefinitions.Empty();
		SpawnedDefinitionIds.Empty();
		WorkerWorld = nullptr;
		PermissionCheck = FPermissionCheck();
		OwnerThreadId = 0;
		bInitialized = false;
		bClientSessionUsable = true;
		bAuthoritativeSessionUsable = true;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::RegisterSpatialDefinition(const FString& DefinitionStableId,
	                                                           ESpatialDefinitionPolicy Policy,
	                                                           FPositionAccessor InPositionAccessor)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (DefinitionStableId.IsEmpty() ||
			static_cast<uint8>(Policy) > static_cast<uint8>(ESpatialDefinitionPolicy::SkipWithoutPosition))
			return EModelResult::Invalid;
		if (!Registry.FindEntityDefinition(DefinitionStableId)) return EModelResult::UnknownSchema;
		const bool bRequiresPosition = Policy == ESpatialDefinitionPolicy::PositionRequired;
		if (bRequiresPosition != static_cast<bool>(InPositionAccessor)) return EModelResult::Invalid;
		if (SpatialDefinitions.Contains(DefinitionStableId)) return EModelResult::Invalid;
		if (SpatialDefinitions.Num() >= static_cast<int32>(FlecsModRegistry::MaxManifestEntries))
			return EModelResult::LimitReached;
		if (SpawnedDefinitionIds.Contains(DefinitionStableId)) return EModelResult::Invalid;

		FSpatialDefinitionRegistration Registration;
		Registration.Policy = Policy;
		Registration.PositionAccessor = MoveTemp(InPositionAccessor);
		SpatialDefinitions.Add(DefinitionStableId, MoveTemp(Registration));
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::SpawnAuthoritative(const FString& DefinitionStableId, uint64 OwnerConnectionToken,
	                                                    TConstArrayView<FInitialComponent> InitialComponents,
	                                                    FNetworkEntityIdentity& OutIdentity)
	{
		OutIdentity = {};
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (InitialComponents.Num() > MaxComponentsPerEntity || EntitiesById.Num() >= MaxEntities)
			return EModelResult::LimitReached;

		const FlecsModRegistry::FEntityDefinition* Definition = Registry.FindEntityDefinition(DefinitionStableId);
		if (!Definition || !Definition->Create) return EModelResult::UnknownSchema;

		TSet<FString> UniqueComponents;
		uint64 TotalBytes = 0;
		for (const FInitialComponent& Initial : InitialComponents)
		{
			if (UniqueComponents.Contains(Initial.StableComponentId) || Initial.StableComponentId.IsEmpty() || Initial.
				Payload.Num() < 0)
				return EModelResult::Invalid;
			const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(Initial.StableComponentId);
			if (!Schema || !IsValidVisibility(Schema->Visibility) || !Schema->Apply ||
				!IsComponentAllowed(DefinitionStableId, Initial.StableComponentId) ||
				static_cast<uint32>(Initial.Payload.Num()) > Schema->MaxEncodedBytes)
				return EModelResult::UnknownSchema;
			UniqueComponents.Add(Initial.StableComponentId);
			TotalBytes += static_cast<uint32>(Initial.Payload.Num());
			if (TotalBytes > Definition->MaxInitialStateBytes || TotalBytes > MaxBatchBytes)
				return EModelResult::LimitReached;
		}

		uint32 EntityId = 0;
		uint16 Generation = 1;
		if (!ReusableEntityIds.IsEmpty())
		{
			EntityId = ReusableEntityIds.Pop(EAllowShrinking::No);
			Generation = NextGenerationById.FindRef(EntityId);
		}
		else
		{
			if (NextNetworkEntityId == 0 || NextGenerationById.Num() >= MaxIdentityHistory) return
				EModelResult::LimitReached;
			EntityId = NextNetworkEntityId++;
			NextGenerationById.Add(EntityId, Generation);
		}
		if (Generation == 0) return EModelResult::LimitReached;

		flecs::entity LocalEntity = WorkerWorld->entity();
		if (!LocalEntity.is_valid())
		{
			RetireEntityId(EntityId);
			return EModelResult::CallbackFailed;
		}
		if (!Definition->Create(static_cast<uint64>(LocalEntity.id())))
		{
			LocalEntity.destruct();
			RetireEntityId(EntityId);
			return EModelResult::CallbackFailed;
		}

		FEntityRecord NewRecord;
		NewRecord.Identity.ServerWorldEpoch = ServerWorldEpoch;
		NewRecord.Identity.NetworkEntityId = EntityId;
		NewRecord.Identity.Generation = Generation;
		NewRecord.Identity.DefinitionStableId = DefinitionStableId;
		NewRecord.OwnerConnectionToken = OwnerConnectionToken;
		NewRecord.LocalEntityToken = static_cast<uint64>(LocalEntity.id());
		for (const FInitialComponent& Initial : InitialComponents)
		{
			const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(Initial.StableComponentId);
			if (!Schema->Apply(NewRecord.LocalEntityToken, Initial.Payload))
			{
				if (Schema->Remove) Schema->Remove(NewRecord.LocalEntityToken);
				for (const FString& AppliedComponentId : NewRecord.PresentComponents)
				{
					const FlecsModRegistry::FComponentSchema* AppliedSchema = Registry.
						FindComponent(AppliedComponentId);
					if (AppliedSchema && AppliedSchema->Remove) AppliedSchema->Remove(NewRecord.LocalEntityToken);
				}
				LocalEntity.destruct();
				RetireEntityId(EntityId);
				return EModelResult::CallbackFailed;
			}
			NewRecord.PresentComponents.Add(Initial.StableComponentId);
			NewRecord.LastComponentTicks.Add(Initial.StableComponentId, 0);
			NewRecord.LastComponentPayloads.Add(Initial.StableComponentId, Initial.Payload);
		}
		EntitiesById.Add(EntityId, MoveTemp(NewRecord));
		SpawnedDefinitionIds.Add(DefinitionStableId);
		OutIdentity = EntitiesById.FindChecked(EntityId).Identity;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::SetOwnerAuthoritative(const FNetworkEntityIdentity& Identity,
	                                                       uint64 NewOwnerConnectionToken)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		FEntityRecord* Record = nullptr;
		if (!ResolveIdentity(Identity, Record)) return EModelResult::StaleIdentity;
		if (Record->OwnerConnectionToken != NewOwnerConnectionToken && Record->StateRevision == MAX_uint64)
			return EModelResult::LimitReached;
		if (Record->OwnerConnectionToken != NewOwnerConnectionToken) ++Record->StateRevision;
		Record->OwnerConnectionToken = NewOwnerConnectionToken;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::AddComponentAuthoritative(const FNetworkEntityIdentity& Identity,
	                                                           const FString& StableComponentId,
	                                                           TConstArrayView<uint8> EncodedPayload,
	                                                           uint32 SimulationTick)
	{
		return SetComponentAuthoritative(Identity, StableComponentId, EncodedPayload, SimulationTick, false);
	}

	EModelResult FWorkerEntityModel::UpdateComponentAuthoritative(const FNetworkEntityIdentity& Identity,
	                                                              const FString& StableComponentId,
	                                                              TConstArrayView<uint8> EncodedPayload,
	                                                              uint32 SimulationTick)
	{
		return SetComponentAuthoritative(Identity, StableComponentId, EncodedPayload, SimulationTick, true);
	}

	EModelResult FWorkerEntityModel::SetComponentAuthoritative(const FNetworkEntityIdentity& Identity,
	                                                           const FString& StableComponentId,
	                                                           TConstArrayView<uint8> EncodedPayload,
	                                                           uint32 SimulationTick,
	                                                           bool bRequireExisting)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (StableComponentId.IsEmpty() || SimulationTick == 0 || EncodedPayload.Num() < 0 ||
			static_cast<uint32>(EncodedPayload.Num()) > MaxPayloadBytesPerRecord)
			return EModelResult::Invalid;
		FEntityRecord* Record = nullptr;
		if (!ResolveIdentity(Identity, Record)) return EModelResult::StaleIdentity;
		const bool bAlreadyPresent = Record->PresentComponents.Contains(StableComponentId);
		if (bRequireExisting ? !bAlreadyPresent : bAlreadyPresent) return EModelResult::Invalid;
		if (!IsComponentAllowed(Record->Identity.DefinitionStableId, StableComponentId)) return
			EModelResult::UnknownSchema;
		const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
		if (!Schema || !Schema->Apply || !Schema->Extract || !Schema->Remove || !IsValidVisibility(Schema->Visibility)
			||
			static_cast<uint32>(EncodedPayload.Num()) > Schema->MaxEncodedBytes)
			return EModelResult::UnknownSchema;
		const uint32 PreviousTick = Record->LastComponentTicks.FindRef(StableComponentId);
		if (SimulationTick <= PreviousTick) return EModelResult::StaleTick;
		if (Record->StateRevision == MAX_uint64) return EModelResult::LimitReached;

		TArray<uint8> PreviousPayload;
		uint64 AggregatePayloadBytes = static_cast<uint32>(EncodedPayload.Num());
		for (const FString& ExistingStableId : Record->PresentComponents)
		{
			const FlecsModRegistry::FComponentSchema* ExistingSchema = Registry.FindComponent(ExistingStableId);
			if (!ExistingSchema || !ExistingSchema->Extract) return EModelResult::UnknownSchema;
			TArray<uint8> ExistingPayload;
			if (!ExistingSchema->Extract(Record->LocalEntityToken, ExistingPayload) ||
				static_cast<uint32>(ExistingPayload.Num()) > ExistingSchema->MaxEncodedBytes ||
				static_cast<uint32>(ExistingPayload.Num()) > MaxPayloadBytesPerRecord)
				return EModelResult::CallbackFailed;
			if (ExistingStableId == StableComponentId)
			{
				PreviousPayload = MoveTemp(ExistingPayload);
				if (bRequireExisting) continue;
			}
			AggregatePayloadBytes += static_cast<uint32>(ExistingPayload.Num());
		}
		if (!bAlreadyPresent && Record->PresentComponents.Num() >= MaxComponentsPerEntity) return
			EModelResult::LimitReached;
		if (AggregatePayloadBytes > MaxBatchBytes) return EModelResult::LimitReached;

		TArray<uint8> CanonicalPayload;
		auto RollBackFailedMutation = [this, Record, Schema, bRequireExisting, &PreviousPayload]() -> EModelResult
		{
			if (bRequireExisting)
			{
				if (Schema->Apply(Record->LocalEntityToken, PreviousPayload)) return EModelResult::CallbackFailed;
			}
			else
			{
				Schema->Remove(Record->LocalEntityToken);
				return EModelResult::CallbackFailed;
			}
			bAuthoritativeSessionUsable = false;
			return EModelResult::SessionInvalidated;
		};
		if (!Schema->Apply(Record->LocalEntityToken, EncodedPayload)) return RollBackFailedMutation();
		if (!Schema->Extract(Record->LocalEntityToken, CanonicalPayload) ||
			static_cast<uint32>(CanonicalPayload.Num()) > Schema->MaxEncodedBytes ||
			static_cast<uint32>(CanonicalPayload.Num()) > MaxPayloadBytesPerRecord ||
			CanonicalPayload.Num() != EncodedPayload.Num() ||
			(!CanonicalPayload.IsEmpty() && FMemory::Memcmp(CanonicalPayload.GetData(), EncodedPayload.GetData(),
			                                                CanonicalPayload.Num()) != 0))
			return RollBackFailedMutation();

		Record->PresentComponents.Add(StableComponentId);
		Record->LastComponentTicks.Add(StableComponentId, SimulationTick);
		Record->LastComponentPayloads.Add(StableComponentId, MoveTemp(CanonicalPayload));
		Record->LastEntityTick = FMath::Max(Record->LastEntityTick, SimulationTick);
		++Record->StateRevision;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::RemoveComponentAuthoritative(const FNetworkEntityIdentity& Identity,
	                                                              const FString& StableComponentId,
	                                                              uint32 SimulationTick)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (StableComponentId.IsEmpty() || SimulationTick == 0) return EModelResult::Invalid;
		FEntityRecord* Record = nullptr;
		if (!ResolveIdentity(Identity, Record)) return EModelResult::StaleIdentity;
		if (!Record->PresentComponents.Contains(StableComponentId)) return EModelResult::Invalid;
		if (!IsComponentAllowed(Record->Identity.DefinitionStableId, StableComponentId)) return
			EModelResult::UnknownSchema;
		const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
		if (!Schema || !Schema->Remove || !IsValidVisibility(Schema->Visibility)) return EModelResult::UnknownSchema;
		if (SimulationTick <= Record->LastComponentTicks.FindRef(StableComponentId)) return EModelResult::StaleTick;
		if (Record->StateRevision == MAX_uint64) return EModelResult::LimitReached;
		Schema->Remove(Record->LocalEntityToken);
		Record->PresentComponents.Remove(StableComponentId);
		Record->LastComponentPayloads.Remove(StableComponentId);
		Record->LastComponentTicks.Add(StableComponentId, SimulationTick);
		Record->LastEntityTick = FMath::Max(Record->LastEntityTick, SimulationTick);
		++Record->StateRevision;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::DespawnAuthoritative(const FNetworkEntityIdentity& Identity, uint32 SimulationTick)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		FEntityRecord* Record = nullptr;
		if (!ResolveIdentity(Identity, Record)) return EModelResult::StaleIdentity;
		if (SimulationTick == 0) return EModelResult::Invalid;
		const uint32 EntityId = Identity.NetworkEntityId;
		RemoveEntity(EntityId);
		RetireEntityId(EntityId);
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::RefreshAuthoritativeComponentChanges(uint32 SimulationTick)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (SimulationTick == 0) return EModelResult::Invalid;
		for (TPair<uint32, FEntityRecord>& Pair : EntitiesById)
		{
			FEntityRecord& Entity = Pair.Value;
			bool bChanged = false;
			uint64 TotalBytes = 0;
			for (const FString& StableId : Entity.PresentComponents)
			{
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableId);
				TArray<uint8> Payload;
				if (!Schema || !Schema->Extract || !Schema->Extract(Entity.LocalEntityToken, Payload) ||
					static_cast<uint32>(Payload.Num()) > Schema->MaxEncodedBytes ||
					static_cast<uint32>(Payload.Num()) > MaxPayloadBytesPerRecord)
				{
					bAuthoritativeSessionUsable = false;
					return EModelResult::SessionInvalidated;
				}
				TotalBytes += static_cast<uint32>(Payload.Num());
				if (TotalBytes > MaxBatchBytes)
				{
					bAuthoritativeSessionUsable = false;
					return EModelResult::SessionInvalidated;
				}
				const TArray<uint8>* Previous = Entity.LastComponentPayloads.Find(StableId);
				if (!Previous || *Previous != Payload)
				{
					if (Entity.StateRevision == MAX_uint64)
					{
						bAuthoritativeSessionUsable = false;
						return EModelResult::SessionInvalidated;
					}
					Entity.LastComponentPayloads.Add(StableId, MoveTemp(Payload));
					Entity.LastComponentTicks.Add(StableId, SimulationTick);
					bChanged = true;
				}
			}
			if (bChanged)
			{
				++Entity.StateRevision;
				Entity.LastEntityTick = FMath::Max(Entity.LastEntityTick, SimulationTick);
			}
		}
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::ValidateBatch(const FStateBatch& Batch) const
	{
		if (Batch.ServerWorldEpoch == 0 || Batch.ServerWorldEpoch != ServerWorldEpoch) return EModelResult::WrongEpoch;
		if (Batch.SimulationTick == 0 || Batch.SimulationTick < LastAppliedBatchTick) return EModelResult::StaleTick;
		if (Batch.Records.Num() > MaxRecordsPerBatch) return EModelResult::LimitReached;

		uint64 TotalBytes = 0;
		TSet<FString> UniqueKeys;
		TMap<uint32, uint16> BatchGenerations;
		TMap<uint32, uint16> BatchDefinitions;
		TSet<uint32> RemovedEntities;
		TSet<uint32> RetainedEntities;
		for (const FStateRecord& Record : Batch.Records)
		{
			if (Record.Identity.ServerWorldEpoch != ServerWorldEpoch || Record.Identity.NetworkEntityId == 0 ||
				Record.Identity.Generation == 0 || Record.DefinitionWireId == 0 ||
				Record.SourceSimulationTick == 0 || Record.SourceSimulationTick > Batch.SimulationTick ||
				!IsValidOperation(Record.Operation) || Record.Payload.Num() < 0 ||
				static_cast<uint32>(Record.Payload.Num()) > MaxPayloadBytesPerRecord)
				return EModelResult::Invalid;
			TotalBytes += static_cast<uint32>(Record.Payload.Num());
			if (TotalBytes > MaxBatchBytes) return EModelResult::LimitReached;
			if ((Record.Operation == EStateOperation::ComponentUpsert || Record.Operation ==
					EStateOperation::ComponentRemove) !=
				(Record.ComponentWireId != 0))
				return EModelResult::Invalid;
			if (Record.Operation != EStateOperation::ComponentUpsert && !Record.Payload.IsEmpty()) return
				EModelResult::Invalid;
			if (Record.Operation == EStateOperation::EntityCreate && Record.ComponentWireId != 0) return
				EModelResult::Invalid;
			if (Record.Operation == EStateOperation::EntityLeaveRelevance && Record.ComponentWireId != 0) return
				EModelResult::Invalid;
			const uint16* SeenGeneration = BatchGenerations.Find(Record.Identity.NetworkEntityId);
			if (SeenGeneration && *SeenGeneration != Record.Identity.Generation) return EModelResult::StaleIdentity;
			BatchGenerations.Add(Record.Identity.NetworkEntityId, Record.Identity.Generation);
			const uint16* SeenDefinition = BatchDefinitions.Find(Record.Identity.NetworkEntityId);
			if (SeenDefinition && *SeenDefinition != Record.DefinitionWireId) return EModelResult::StaleIdentity;
			BatchDefinitions.Add(Record.Identity.NetworkEntityId, Record.DefinitionWireId);
			const bool bRemoval = Record.Operation == EStateOperation::EntityDespawn ||
				Record.Operation == EStateOperation::EntityLeaveRelevance;
			if (bRemoval
				    ? RetainedEntities.Contains(Record.Identity.NetworkEntityId)
				    : RemovedEntities.Contains(Record.Identity.NetworkEntityId)) return EModelResult::Invalid;
			if (bRemoval) RemovedEntities.Add(Record.Identity.NetworkEntityId);
			else RetainedEntities.Add(Record.Identity.NetworkEntityId);
			const FString UniqueKey = FString::Printf(TEXT("%u:%u:%u"), Record.Identity.NetworkEntityId,
			                                          Record.Identity.Generation, Record.ComponentWireId);
			if (UniqueKeys.Contains(UniqueKey)) return EModelResult::Invalid;
			UniqueKeys.Add(UniqueKey);
		}
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::ApplyClientBatch(const FStateBatch& Batch,
	                                                  const FlecsModRegistry::FSessionMapping& PeerWireMapping)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (bAuthoritative) return EModelResult::WrongRole;
		if (!bClientSessionUsable) return EModelResult::SessionInvalidated;
		if (!PeerWireMapping.IsValid()) return EModelResult::UnknownSchema;
		const EModelResult Validation = ValidateBatch(Batch);
		if (Validation != EModelResult::Accepted) return Validation;
		// Preflight the entire mapping and schema surface before invoking any mod callback.
		for (const FStateRecord& Record : Batch.Records)
		{
			FlecsModRegistry::FSessionWireEntry DefinitionEntry;
			if (!PeerWireMapping.FindEntryByWireId(Record.DefinitionWireId, DefinitionEntry) ||
				DefinitionEntry.Kind != FlecsModRegistry::EEntryKind::EntityDefinition ||
				!Registry.FindEntityDefinition(DefinitionEntry.StableId))
				return EModelResult::UnknownSchema;
			if (Record.Operation == EStateOperation::ComponentUpsert || Record.Operation ==
				EStateOperation::ComponentRemove)
			{
				FlecsModRegistry::FSessionWireEntry ComponentEntry;
				if (!PeerWireMapping.FindEntryByWireId(Record.ComponentWireId, ComponentEntry) ||
					ComponentEntry.Kind != FlecsModRegistry::EEntryKind::Component ||
					!IsComponentAllowed(DefinitionEntry.StableId, ComponentEntry.StableId))
					return EModelResult::UnknownSchema;
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(ComponentEntry.StableId);
				if (!Schema || !IsValidVisibility(Schema->Visibility) || static_cast<uint32>(Record.Payload.Num()) >
					Schema->MaxEncodedBytes ||
					(Record.Operation == EStateOperation::ComponentUpsert && !Schema->Apply) ||
					(Record.Operation == EStateOperation::ComponentRemove && !Schema->Remove))
					return EModelResult::UnknownSchema;
			}
		}
		TSet<uint32> PresentEntityIds;
		TMap<uint32, TSet<uint16>> PresentComponentWires;

		for (const FStateRecord& Record : Batch.Records)
		{
			FlecsModRegistry::FSessionWireEntry DefinitionEntry;
			if (!PeerWireMapping.FindEntryByWireId(Record.DefinitionWireId, DefinitionEntry) ||
				DefinitionEntry.Kind != FlecsModRegistry::EEntryKind::EntityDefinition)
				return EModelResult::UnknownSchema;
			FStateRecord LocalRecord = Record;
			LocalRecord.Identity.DefinitionStableId = DefinitionEntry.StableId;
			if (Record.Operation != EStateOperation::EntityDespawn && Record.Operation !=
				EStateOperation::EntityLeaveRelevance)
			{
				PresentEntityIds.Add(Record.Identity.NetworkEntityId);
				if (Record.Operation == EStateOperation::ComponentUpsert)
					PresentComponentWires.FindOrAdd(Record.Identity.NetworkEntityId).Add(Record.ComponentWireId);
			}
			const EModelResult Result = ApplyRecord(LocalRecord, Batch.SimulationTick, PeerWireMapping);
			if (Result != EModelResult::Accepted)
			{
				// Mod callbacks mutate Flecs directly and cannot be rolled back generically.
				// Mark the client state unusable so the bridge closes/resets the session.
				bClientSessionUsable = false;
				return EModelResult::SessionInvalidated;
			}
		}
		if (Batch.bCompleteStateSnapshot)
		{
			TArray<uint32> ExistingIds;
			EntitiesById.GenerateKeyArray(ExistingIds);
			for (uint32 ExistingId : ExistingIds)
			{
				if (!PresentEntityIds.Contains(ExistingId))
				{
					const FEntityRecord* Departing = EntitiesById.Find(ExistingId);
					if (Departing)
					{
						FReplicaHistory& History = ReplicaHistoryById.FindOrAdd(ExistingId);
						History.Generation = Departing->Identity.Generation;
						History.DefinitionStableId = Departing->Identity.DefinitionStableId;
						History.LastEntityTick = Departing->LastEntityTick;
						History.LastComponentTicks = Departing->LastComponentTicks;
						History.LastComponentPayloads = Departing->LastComponentPayloads;
					}
					RemoveEntity(ExistingId);
					// Relevance exit clears the local replica but does not consume a
					// server generation. The same identity may become relevant later.
					continue;
				}
				FEntityRecord* Existing = EntitiesById.Find(ExistingId);
				if (!Existing) continue;
				const TSet<uint16>* PresentWires = PresentComponentWires.Find(ExistingId);
				TArray<FString> ExistingComponents = Existing->PresentComponents.Array();
				for (const FString& StableComponentId : ExistingComponents)
				{
					uint16 WireId = 0;
					if (!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::Component, StableComponentId, WireId)
						||
						(PresentWires && PresentWires->Contains(WireId)))
						continue;
					const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
					if (Schema && Schema->Remove) Schema->Remove(Existing->LocalEntityToken);
					Existing->PresentComponents.Remove(StableComponentId);
					Existing->LastComponentPayloads.Remove(StableComponentId);
				}
			}
		}
		LastAppliedBatchTick = Batch.SimulationTick;
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::ApplyRecord(const FStateRecord& Record, uint32 BatchTick,
	                                             const FlecsModRegistry::FSessionMapping& PeerWireMapping)
	{
		FEntityRecord* Existing = EntitiesById.Find(Record.Identity.NetworkEntityId);
		if (Existing && Existing->Identity.Generation != Record.Identity.Generation)
		{
			if (Record.Identity.Generation < Existing->Identity.Generation) return EModelResult::StaleIdentity;
			// A despawn must match the currently live generation; an unseen future
			// despawn cannot be allowed to retire or remove another incarnation.
			if (Record.Operation == EStateOperation::EntityDespawn || Record.Operation ==
				EStateOperation::EntityLeaveRelevance)
				return EModelResult::StaleIdentity;
			const uint16 RetiredGeneration = Existing->Identity.Generation;
			RemoveEntity(Record.Identity.NetworkEntityId);
			ReplicaHistoryById.Remove(Record.Identity.NetworkEntityId);
			NextGenerationById.Add(Record.Identity.NetworkEntityId, RetiredGeneration);
			RetiredEntityIds.Add(Record.Identity.NetworkEntityId);
			Existing = nullptr;
		}
		if (Record.Operation == EStateOperation::EntityDespawn || Record.Operation ==
			EStateOperation::EntityLeaveRelevance)
		{
			if (!Existing && Record.Operation == EStateOperation::EntityDespawn)
			{
				const FDespawnTombstone* Tombstone = DespawnTombstonesById.Find(Record.Identity.NetworkEntityId);
				if (Tombstone && Tombstone->ServerWorldEpoch == Record.Identity.ServerWorldEpoch &&
					Tombstone->Generation == Record.Identity.Generation &&
					Tombstone->SimulationTick == Record.SourceSimulationTick &&
					Tombstone->DefinitionStableId == Record.Identity.DefinitionStableId)
					return EModelResult::Accepted;
				return EModelResult::StaleIdentity;
			}
			if (!Existing && Record.Operation == EStateOperation::EntityLeaveRelevance)
			{
				FReplicaHistory* History = ReplicaHistoryById.Find(Record.Identity.NetworkEntityId);
				if (History && History->Generation == Record.Identity.Generation &&
					History->DefinitionStableId == Record.Identity.DefinitionStableId &&
					Record.SourceSimulationTick >= History->LastEntityTick)
				{
					History->LastEntityTick = Record.SourceSimulationTick;
					return EModelResult::Accepted;
				}
				return EModelResult::StaleIdentity;
			}
			if (!Existing || Existing->Identity.DefinitionStableId != Record.Identity.DefinitionStableId ||
				Record.SourceSimulationTick < Existing->LastEntityTick)
				return EModelResult::StaleIdentity;
			if (Record.Operation == EStateOperation::EntityLeaveRelevance)
			{
				FReplicaHistory& History = ReplicaHistoryById.FindOrAdd(Record.Identity.NetworkEntityId);
				History.Generation = Existing->Identity.Generation;
				History.DefinitionStableId = Existing->Identity.DefinitionStableId;
				History.LastEntityTick = FMath::Max(Existing->LastEntityTick, Record.SourceSimulationTick);
				History.LastComponentTicks = Existing->LastComponentTicks;
				History.LastComponentPayloads = Existing->LastComponentPayloads;
			}
			else
			{
				if (!DespawnTombstonesById.Contains(Record.Identity.NetworkEntityId) &&
					DespawnTombstonesById.Num() >= MaxIdentityHistory)
					return EModelResult::LimitReached;
				FDespawnTombstone& Tombstone = DespawnTombstonesById.FindOrAdd(Record.Identity.NetworkEntityId);
				Tombstone.ServerWorldEpoch = Record.Identity.ServerWorldEpoch;
				Tombstone.Generation = Record.Identity.Generation;
				Tombstone.SimulationTick = Record.SourceSimulationTick;
				Tombstone.DefinitionStableId = Record.Identity.DefinitionStableId;
				ReplicaHistoryById.Remove(Record.Identity.NetworkEntityId);
			}
			RemoveEntity(Record.Identity.NetworkEntityId);
			if (Record.Operation == EStateOperation::EntityDespawn) RetireEntityId(Record.Identity.NetworkEntityId);
			return EModelResult::Accepted;
		}

		if (!Existing && (Record.Operation == EStateOperation::EntityCreate || Record.Operation ==
			EStateOperation::ComponentUpsert))
		{
			const EModelResult CreateResult = CreateClientEntity(Record.Identity, Record.Identity.DefinitionStableId,
			                                                     Existing);
			if (CreateResult != EModelResult::Accepted) return CreateResult;
		}
		if (!Existing) return EModelResult::StaleIdentity;
		if (Existing->Identity.DefinitionStableId != Record.Identity.DefinitionStableId) return
			EModelResult::StaleIdentity;

		if (Record.Operation == EStateOperation::EntityCreate)
		{
			if (Record.SourceSimulationTick < Existing->LastEntityTick) return EModelResult::StaleTick;
			Existing->LastEntityTick = FMath::Max(Existing->LastEntityTick, Record.SourceSimulationTick);
			return EModelResult::Accepted;
		}

		FlecsModRegistry::FSessionWireEntry ComponentEntry;
		if (!PeerWireMapping.FindEntryByWireId(Record.ComponentWireId, ComponentEntry) || ComponentEntry.Kind !=
			FlecsModRegistry::EEntryKind::Component)
			return EModelResult::UnknownSchema;
		const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(ComponentEntry.StableId);
		const FlecsModRegistry::FEntityDefinition* Definition = nullptr;
		Definition = Registry.FindEntityDefinition(Existing->Identity.DefinitionStableId);
		if (!Schema || !IsValidVisibility(Schema->Visibility) || !Definition ||
			!IsComponentAllowed(Existing->Identity.DefinitionStableId, ComponentEntry.StableId))
			return EModelResult::UnknownSchema;
		const uint32 PreviousTick = Existing->LastComponentTicks.FindRef(ComponentEntry.StableId);
		if (Record.SourceSimulationTick > BatchTick || Record.SourceSimulationTick < PreviousTick) return
			EModelResult::StaleTick;
		if (Record.SourceSimulationTick == PreviousTick)
		{
			// Complete snapshots may repeat unchanged component records. Equal ticks
			// are idempotent when they match a live component. After relevance exit,
			// the retained exact value is still safe to reapply to a fresh local entity.
			if (Record.Operation == EStateOperation::ComponentUpsert)
			{
				const bool bComponentIsPresent = Existing->PresentComponents.Contains(ComponentEntry.StableId);
				const TArray<uint8>* PreviousPayload = Existing->LastComponentPayloads.Find(ComponentEntry.StableId);
				if (bComponentIsPresent && PreviousPayload && *PreviousPayload == Record.Payload)
					return EModelResult::Accepted;
				if (!bComponentIsPresent && PreviousPayload && *PreviousPayload == Record.Payload)
				{
					// Continue to Apply below: this entity was recreated after relevance exit.
				}
				else
				{
					return EModelResult::StaleTick;
				}
			}
			else if (Record.Operation == EStateOperation::ComponentRemove &&
				!Existing->PresentComponents.Contains(ComponentEntry.StableId))
				return EModelResult::Accepted;
			else
				return EModelResult::StaleTick;
		}

		if (Record.Operation == EStateOperation::ComponentUpsert)
		{
			if (!Schema->Apply || static_cast<uint32>(Record.Payload.Num()) > Schema->MaxEncodedBytes) return
				EModelResult::CallbackFailed;
			if (!Schema->Apply(Existing->LocalEntityToken, Record.Payload))
			{
				if (Schema->Remove) Schema->Remove(Existing->LocalEntityToken);
				return EModelResult::CallbackFailed;
			}
			Existing->PresentComponents.Add(ComponentEntry.StableId);
			Existing->LastComponentPayloads.Add(ComponentEntry.StableId, Record.Payload);
		}
		else if (Record.Operation == EStateOperation::ComponentRemove)
		{
			if (!Schema->Remove) return EModelResult::UnknownSchema;
			Schema->Remove(Existing->LocalEntityToken);
			Existing->PresentComponents.Remove(ComponentEntry.StableId);
			Existing->LastComponentPayloads.Remove(ComponentEntry.StableId);
		}
		Existing->LastComponentTicks.Add(ComponentEntry.StableId, Record.SourceSimulationTick);
		Existing->LastEntityTick = FMath::Max(Existing->LastEntityTick, Record.SourceSimulationTick);
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::CreateClientEntity(const FNetworkEntityIdentity& Identity,
	                                                    const FString& DefinitionStableId, FEntityRecord*& OutRecord)
	{
		OutRecord = nullptr;
		if (EntitiesById.Num() >= MaxEntities || Identity.ServerWorldEpoch != ServerWorldEpoch ||
			Identity.NetworkEntityId == 0 || Identity.Generation == 0 ||
			(!NextGenerationById.Contains(Identity.NetworkEntityId) && NextGenerationById.Num() >= MaxIdentityHistory))
			return EModelResult::LimitReached;
		const FlecsModRegistry::FEntityDefinition* Definition = Registry.FindEntityDefinition(DefinitionStableId);
		if (!Definition || !Definition->Create) return EModelResult::UnknownSchema;

		const uint16 HighestGeneration = NextGenerationById.FindRef(Identity.NetworkEntityId);
		if (HighestGeneration != 0 && Identity.Generation < HighestGeneration) return EModelResult::StaleIdentity;
		if (const FDespawnTombstone* Tombstone = DespawnTombstonesById.Find(Identity.NetworkEntityId))
		{
			if (Identity.Generation <= Tombstone->Generation) return EModelResult::StaleIdentity;
		}
		if (const FReplicaHistory* History = ReplicaHistoryById.Find(Identity.NetworkEntityId))
		{
			if (History->Generation == Identity.Generation && History->DefinitionStableId != DefinitionStableId)
				return EModelResult::StaleIdentity;
		}
		flecs::entity LocalEntity = WorkerWorld->entity();
		if (!LocalEntity.is_valid() || !Definition->Create(static_cast<uint64>(LocalEntity.id())))
		{
			if (LocalEntity.is_valid()) LocalEntity.destruct();
			return EModelResult::CallbackFailed;
		}
		FEntityRecord NewRecord;
		NewRecord.Identity = Identity;
		NewRecord.Identity.DefinitionStableId = DefinitionStableId;
		NewRecord.LocalEntityToken = static_cast<uint64>(LocalEntity.id());
		if (FReplicaHistory* History = ReplicaHistoryById.Find(Identity.NetworkEntityId))
		{
			if (History->Generation == Identity.Generation)
			{
				NewRecord.LastEntityTick = History->LastEntityTick;
				NewRecord.LastComponentTicks = History->LastComponentTicks;
				NewRecord.LastComponentPayloads = History->LastComponentPayloads;
			}
			else
			{
				ReplicaHistoryById.Remove(Identity.NetworkEntityId);
			}
		}
		NextGenerationById.Add(Identity.NetworkEntityId, Identity.Generation);
		RetiredEntityIds.Remove(Identity.NetworkEntityId);
		OutRecord = &EntitiesById.Add(Identity.NetworkEntityId, MoveTemp(NewRecord));
		if (const FDespawnTombstone* Tombstone = DespawnTombstonesById.Find(Identity.NetworkEntityId))
		{
			if (Identity.Generation > Tombstone->Generation) DespawnTombstonesById.Remove(Identity.NetworkEntityId);
		}
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::BuildAuthoritativeSnapshot(uint32 SimulationTick,
	                                                            const FPeerReplicationContext& PeerContext,
	                                                            const FlecsModRegistry::FSessionMapping&
	                                                            PeerWireMapping, FStateBatch& OutBatch)
	{
		OutBatch = {};
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (SimulationTick == 0 || PeerContext.ConnectionToken == 0 ||
			PeerContext.RelevantEntityKeys.Num() > MaxEntities || !PeerWireMapping.IsValid())
			return EModelResult::Invalid;
		FStateBatch Candidate;
		Candidate.ServerWorldEpoch = ServerWorldEpoch;
		Candidate.SimulationTick = SimulationTick;
		Candidate.bCompleteStateSnapshot = true;
		TArray<uint32> SortedIds;
		EntitiesById.GenerateKeyArray(SortedIds);
		SortedIds.Sort();
		uint64 TotalBytes = 0;
		for (uint32 EntityId : SortedIds)
		{
			const FEntityRecord& Entity = EntitiesById.FindChecked(EntityId);
			if (!PeerContext.Includes(Entity.Identity)) continue;
			if (Candidate.Records.Num() >= MaxRecordsPerBatch) return EModelResult::LimitReached;
			FStateRecord& CreateRecord = Candidate.Records.AddDefaulted_GetRef();
			CreateRecord.Identity = Entity.Identity;
			CreateRecord.Identity.DefinitionStableId.Reset();
			if (!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::EntityDefinition,
			                                Entity.Identity.DefinitionStableId, CreateRecord.DefinitionWireId))
				return EModelResult::UnknownSchema;
			const uint16 DefinitionWireId = CreateRecord.DefinitionWireId;
			CreateRecord.SourceSimulationTick = SimulationTick;
			CreateRecord.Operation = EStateOperation::EntityCreate;
			TotalBytes += EstimatedReplicationRecordHeaderBytes;
			if (TotalBytes > MaxBatchBytes) return EModelResult::LimitReached;
			TArray<FString> SortedComponents = Entity.PresentComponents.Array();
			SortedComponents.Sort();
			for (const FString& StableComponentId : SortedComponents)
			{
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
				if (!Schema || !IsValidVisibility(Schema->Visibility)) return EModelResult::UnknownSchema;
				if (Schema->Visibility == FlecsModRegistry::EVisibilityPolicy::OwnerOnly &&
					Entity.OwnerConnectionToken != PeerContext.ConnectionToken)
					continue;
				if (Candidate.Records.Num() >= MaxRecordsPerBatch) return EModelResult::LimitReached;
				if (!Schema->Extract) return EModelResult::UnknownSchema;
				FStateRecord& Record = Candidate.Records.AddDefaulted_GetRef();
				Record.Identity = Entity.Identity;
				Record.Identity.DefinitionStableId.Reset();
				Record.DefinitionWireId = DefinitionWireId;
				if (!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::Component,
				                                StableComponentId, Record.ComponentWireId))
					return EModelResult::UnknownSchema;
				Record.SourceSimulationTick = SimulationTick;
				Record.Operation = EStateOperation::ComponentUpsert;
				if (!Schema->Extract(Entity.LocalEntityToken, Record.Payload) || Record.Payload.Num() < 0 ||
					static_cast<uint32>(Record.Payload.Num()) > Schema->MaxEncodedBytes ||
					static_cast<uint32>(Record.Payload.Num()) > MaxPayloadBytesPerRecord)
				{
					return EModelResult::CallbackFailed;
				}
				TotalBytes += EstimatedReplicationRecordHeaderBytes + static_cast<uint32>(Record.Payload.Num());
				if (TotalBytes > MaxBatchBytes)
					return EModelResult::LimitReached;
			}
		}
		OutBatch = MoveTemp(Candidate);
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::BuildAuthoritativeDelta(uint32 SimulationTick,
	                                                         const FPeerReplicationContext& PeerContext,
	                                                         const FlecsModRegistry::FSessionMapping& PeerWireMapping,
	                                                         TConstArrayView<FSelectedEntityComponents> DueSelection,
	                                                         FStateBatch& OutBatch)
	{
		OutBatch = {};
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (SimulationTick == 0 || PeerContext.ConnectionToken == 0 ||
			PeerContext.RelevantEntityKeys.Num() > MaxEntities || DueSelection.Num() > MaxEntities || !PeerWireMapping.
			IsValid())
			return EModelResult::Invalid;

		FPeerDeltaState* ExistingPeerState = PeerDeltaStates.Find(PeerContext.ConnectionToken);
		if (ExistingPeerState && ExistingPeerState->PendingBatch.PublicationRevision != 0)
			return EModelResult::RateLimited;
		if (!ExistingPeerState && PeerDeltaStates.Num() >= MaxPeerReplicationStates) return EModelResult::LimitReached;

		TMap<uint64, const FSelectedEntityComponents*> DueByKey;
		for (const FSelectedEntityComponents& Selection : DueSelection)
		{
			if (!Selection.Identity.IsValid() || Selection.Identity.ServerWorldEpoch != ServerWorldEpoch ||
				!PeerContext.Includes(Selection.Identity) || Selection.DueComponentStableIds.Num() >
				MaxComponentsPerEntity)
				return EModelResult::Invalid;
			const uint64 ViewKey = FPeerReplicationContext::MakeEntityViewKey(
				Selection.Identity.NetworkEntityId, Selection.Identity.Generation);
			if (DueByKey.Contains(ViewKey)) return EModelResult::Invalid;
			const FEntityRecord* Live = EntitiesById.Find(Selection.Identity.NetworkEntityId);
			if (!Live || Live->Identity.Generation != Selection.Identity.Generation ||
				Live->Identity.DefinitionStableId != Selection.Identity.DefinitionStableId)
				return EModelResult::StaleIdentity;
			TSet<FString> UniqueComponentIds;
			for (const FString& StableId : Selection.DueComponentStableIds)
			{
				if (StableId.IsEmpty() || UniqueComponentIds.Contains(StableId) ||
					!Live->PresentComponents.Contains(StableId) ||
					!IsComponentAllowed(Live->Identity.DefinitionStableId, StableId))
					return EModelResult::Invalid;
				UniqueComponentIds.Add(StableId);
			}
			DueByKey.Add(ViewKey, &Selection);
		}

		for (uint64 ViewKey : PeerContext.RelevantEntityKeys)
		{
			const uint32 EntityId = static_cast<uint32>(ViewKey >> 16);
			const uint16 Generation = static_cast<uint16>(ViewKey & 0xffff);
			if (ViewKey != FPeerReplicationContext::MakeEntityViewKey(EntityId, Generation) ||
				EntityId == 0 || Generation == 0)
				return EModelResult::Invalid;
		}

		FPeerDeltaState CandidateState = ExistingPeerState ? *ExistingPeerState : FPeerDeltaState();
		for (const TPair<uint64, const FSelectedEntityComponents*>& Due : DueByKey)
		{
			const FNetworkEntityIdentity& Identity = Due.Value->Identity;
			const FPeerEntityBaseline* Baseline = CandidateState.AcknowledgedEntities.Find(Identity.NetworkEntityId);
			if (Baseline && Baseline->Identity.Generation != Identity.Generation && PeerContext.Includes(
				Baseline->Identity))
			{
				// The old generation's budget-deferred exit must be acknowledged first.
				return EModelResult::StaleIdentity;
			}
		}
		CandidateState.PendingEntities = CandidateState.AcknowledgedEntities;
		CandidateState.PendingPayloadBytes = CandidateState.AcknowledgedPayloadBytes;
		CandidateState.PendingBatch = {};
		FStateBatch CandidateBatch;
		CandidateBatch.ServerWorldEpoch = ServerWorldEpoch;
		CandidateBatch.SimulationTick = SimulationTick;
		CandidateBatch.bCompleteStateSnapshot = false;
		uint64 TotalRecordBytes = 0;

		auto AddRecord = [&CandidateBatch, &TotalRecordBytes](FStateRecord&& Record) -> bool
		{
			if (CandidateBatch.Records.Num() >= MaxRecordsPerBatch) return false;
			TotalRecordBytes += EstimatedReplicationRecordHeaderBytes + static_cast<uint32>(Record.Payload.Num());
			if (TotalRecordBytes > MaxBatchBytes) return false;
			CandidateBatch.Records.Add(MoveTemp(Record));
			return true;
		};
		auto MakeEntityRecord = [this, SimulationTick, &PeerWireMapping](const FNetworkEntityIdentity& Identity,
		                                                                 EStateOperation Operation,
		                                                                 FStateRecord& OutRecord) -> bool
		{
			OutRecord = {};
			OutRecord.Identity = Identity;
			OutRecord.Identity.DefinitionStableId.Reset();
			if (!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::EntityDefinition,
			                                Identity.DefinitionStableId, OutRecord.DefinitionWireId))
				return false;
			OutRecord.SourceSimulationTick = SimulationTick;
			OutRecord.Operation = Operation;
			return true;
		};
		auto RecalculatePendingLimits = [&CandidateState]() -> bool
		{
			if (CandidateState.PendingEntities.Num() > MaxEntities) return false;
			uint64 Bytes = 0;
			int32 Components = 0;
			for (const TPair<uint32, FPeerEntityBaseline>& EntityPair : CandidateState.PendingEntities)
			{
				Components += EntityPair.Value.Components.Num();
				for (const TPair<FString, FPeerComponentBaseline>& ComponentPair : EntityPair.Value.Components)
					Bytes += static_cast<uint32>(ComponentPair.Value.Payload.Num());
			}
			CandidateState.PendingPayloadBytes = Bytes;
			return Components <= MaxPeerKnownComponents && Bytes <= MaxBatchBytes;
		};

		TArray<uint32> SortedBaselineIds;
		CandidateState.AcknowledgedEntities.GenerateKeyArray(SortedBaselineIds);
		SortedBaselineIds.Sort();
		TSet<uint32> DeferredNewGenerationIds;
		for (uint32 EntityId : SortedBaselineIds)
		{
			const FPeerEntityBaseline* Baseline = CandidateState.PendingEntities.Find(EntityId);
			if (!Baseline) continue;
			const bool bStillRelevant = PeerContext.Includes(Baseline->Identity);
			const FEntityRecord* Live = EntitiesById.Find(EntityId);
			const bool bSameLiveIdentity = Live && Live->Identity.Generation == Baseline->Identity.Generation &&
				Live->Identity.ServerWorldEpoch == Baseline->Identity.ServerWorldEpoch &&
				Live->Identity.DefinitionStableId == Baseline->Identity.DefinitionStableId;
			// Retained keys include exits deferred by the scheduler's budget. Keep
			// their acknowledged baseline until that exit is selected, even if the
			// worker has already despawned or replaced the entity.
			if (bStillRelevant) continue;

			const bool bNewGenerationLive = Live && Live->Identity.Generation != Baseline->Identity.Generation &&
				PeerContext.Includes(Live->Identity);
			FStateRecord TransitionRecord;
			if (!MakeEntityRecord(Baseline->Identity,
			                      bSameLiveIdentity
				                      ? EStateOperation::EntityLeaveRelevance
				                      : EStateOperation::EntityDespawn, TransitionRecord))
				return EModelResult::UnknownSchema;
			if (!AddRecord(MoveTemp(TransitionRecord))) return EModelResult::LimitReached;
			CandidateState.PendingEntities.Remove(EntityId);
			if (bNewGenerationLive) DeferredNewGenerationIds.Add(EntityId);
		}

		TArray<uint64> SortedRelevantKeys = PeerContext.RelevantEntityKeys.Array();
		SortedRelevantKeys.Sort();
		for (uint64 ViewKey : SortedRelevantKeys)
		{
			const uint32 EntityId = static_cast<uint32>(ViewKey >> 16);
			if (DeferredNewGenerationIds.Contains(EntityId)) continue;
			const FEntityRecord* Entity = EntitiesById.Find(EntityId);
			if (!Entity || Entity->Identity.Generation != static_cast<uint16>(ViewKey & 0xffff)) continue;
			FPeerEntityBaseline* Baseline = CandidateState.PendingEntities.Find(EntityId);
			const bool bNewToPeer = !Baseline || Baseline->Identity.Generation != Entity->Identity.Generation ||
				Baseline->Identity.ServerWorldEpoch != Entity->Identity.ServerWorldEpoch ||
				Baseline->Identity.DefinitionStableId != Entity->Identity.DefinitionStableId;
			if (bNewToPeer)
			{
				FStateRecord CreateRecord;
				if (!MakeEntityRecord(Entity->Identity, EStateOperation::EntityCreate, CreateRecord)) return
					EModelResult::UnknownSchema;
				if (!AddRecord(MoveTemp(CreateRecord))) return EModelResult::LimitReached;
				FPeerEntityBaseline NewBaseline;
				NewBaseline.Identity = Entity->Identity;
				Baseline = &CandidateState.PendingEntities.Add(EntityId, MoveTemp(NewBaseline));
			}

			const FSelectedEntityComponents* Selection = DueByKey.FindRef(ViewKey);
			if (!bNewToPeer && !Selection)
			{
				// Retained entities were not included in this scheduler budget.
				// Leave their component baseline intact until they are due.
				continue;
			}
			TArray<FString> PresentComponentIds = Entity->PresentComponents.Array();
			PresentComponentIds.Sort();
			for (const FString& StableId : PresentComponentIds)
			{
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableId);
				if (!Schema || !IsValidVisibility(Schema->Visibility) ||
					!IsComponentAllowed(Entity->Identity.DefinitionStableId, StableId))
					return EModelResult::UnknownSchema;
				const bool bVisible = Schema->Visibility != FlecsModRegistry::EVisibilityPolicy::OwnerOnly ||
					Entity->OwnerConnectionToken == PeerContext.ConnectionToken;
				FPeerComponentBaseline* ComponentBaseline = Baseline->Components.Find(StableId);
				if (!bVisible)
				{
					if (ComponentBaseline)
					{
						if (!Schema->Remove) return EModelResult::UnknownSchema;
						FStateRecord RemoveRecord;
						if (!MakeEntityRecord(Entity->Identity, EStateOperation::ComponentRemove, RemoveRecord) ||
							!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::Component, StableId,
							                            RemoveRecord.ComponentWireId))
							return EModelResult::UnknownSchema;
						if (!AddRecord(MoveTemp(RemoveRecord))) return EModelResult::LimitReached;
						Baseline->Components.Remove(StableId);
					}
					continue;
				}
				const bool bDue = bNewToPeer || (Selection && (Selection->bAllVisibleComponentsDue ||
					Selection->DueComponentStableIds.Contains(StableId)));
				if (!bDue) continue;
				if (!Schema->Extract) return EModelResult::UnknownSchema;
				FStateRecord UpsertRecord;
				if (!MakeEntityRecord(Entity->Identity, EStateOperation::ComponentUpsert, UpsertRecord) ||
					!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::Component, StableId,
					                            UpsertRecord.ComponentWireId))
					return EModelResult::UnknownSchema;
				if (!Schema->Extract(Entity->LocalEntityToken, UpsertRecord.Payload) ||
					static_cast<uint32>(UpsertRecord.Payload.Num()) > Schema->MaxEncodedBytes ||
					static_cast<uint32>(UpsertRecord.Payload.Num()) > MaxPayloadBytesPerRecord)
					return EModelResult::CallbackFailed;
				if (ComponentBaseline && ComponentBaseline->Payload == UpsertRecord.Payload) continue;
				FPeerComponentBaseline NewComponentBaseline;
				NewComponentBaseline.Payload = UpsertRecord.Payload;
				NewComponentBaseline.SourceSimulationTick = SimulationTick;
				if (!AddRecord(MoveTemp(UpsertRecord))) return EModelResult::LimitReached;
				Baseline->Components.Add(StableId, MoveTemp(NewComponentBaseline));
			}
			TArray<FString> BaselineComponentIds;
			Baseline->Components.GenerateKeyArray(BaselineComponentIds);
			for (const FString& StableId : BaselineComponentIds)
			{
				if (Entity->PresentComponents.Contains(StableId)) continue;
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableId);
				if (!Schema || !Schema->Remove) return EModelResult::UnknownSchema;
				FStateRecord RemoveRecord;
				if (!MakeEntityRecord(Entity->Identity, EStateOperation::ComponentRemove, RemoveRecord) ||
					!PeerWireMapping.FindWireId(FlecsModRegistry::EEntryKind::Component, StableId,
					                            RemoveRecord.ComponentWireId))
					return EModelResult::UnknownSchema;
				if (!AddRecord(MoveTemp(RemoveRecord))) return EModelResult::LimitReached;
				Baseline->Components.Remove(StableId);
			}
		}
		if (!RecalculatePendingLimits()) return EModelResult::LimitReached;

		if (CandidateBatch.Records.IsEmpty())
		{
			OutBatch = MoveTemp(CandidateBatch);
			return EModelResult::Accepted;
		}
		if (NextPublicationRevision == 0 || NextPublicationRevision == MAX_uint64) return EModelResult::LimitReached;
		CandidateBatch.PublicationRevision = NextPublicationRevision++;
		CandidateState.PendingBatch = CandidateBatch;
		if (!ExistingPeerState) ExistingPeerState = &PeerDeltaStates.
			Add(PeerContext.ConnectionToken, FPeerDeltaState());
		*ExistingPeerState = MoveTemp(CandidateState);
		OutBatch = MoveTemp(CandidateBatch);
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::AcknowledgeAuthoritativeDelta(uint64 ConnectionToken,
	                                                               uint64 InServerWorldEpoch,
	                                                               uint64 PublicationRevision)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (InServerWorldEpoch != ServerWorldEpoch) return EModelResult::WrongEpoch;
		if (ConnectionToken == 0 || PublicationRevision == 0) return EModelResult::Invalid;
		FPeerDeltaState* State = PeerDeltaStates.Find(ConnectionToken);
		if (!State || State->PendingBatch.PublicationRevision != PublicationRevision) return
			EModelResult::StaleIdentity;
		State->AcknowledgedEntities = MoveTemp(State->PendingEntities);
		State->AcknowledgedPayloadBytes = State->PendingPayloadBytes;
		State->PendingEntities.Empty();
		State->PendingPayloadBytes = 0;
		State->PendingBatch = {};
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::DiscardAuthoritativeDelta(uint64 ConnectionToken,
	                                                           uint64 InServerWorldEpoch, uint64 PublicationRevision,
	                                                           bool bForgetPeer)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (InServerWorldEpoch != ServerWorldEpoch) return EModelResult::WrongEpoch;
		if (ConnectionToken == 0 || (PublicationRevision == 0 && !bForgetPeer)) return EModelResult::Invalid;
		FPeerDeltaState* State = PeerDeltaStates.Find(ConnectionToken);
		if (bForgetPeer && PublicationRevision == 0)
		{
			PeerDeltaStates.Remove(ConnectionToken);
			CommandWindows.Remove(ConnectionToken);
			return EModelResult::Accepted;
		}
		if (!State || State->PendingBatch.PublicationRevision != PublicationRevision) return
			EModelResult::StaleIdentity;
		if (bForgetPeer)
		{
			PeerDeltaStates.Remove(ConnectionToken);
			CommandWindows.Remove(ConnectionToken);
			return EModelResult::Accepted;
		}
		State->PendingEntities.Empty();
		State->PendingPayloadBytes = 0;
		State->PendingBatch = {};
		return EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::BuildSpatialMetadata(uint32 SimulationTick,
	                                                      TArray<FSpatialEntityMetadata>& OutMetadata,
	                                                      TArray<FSpatialMetadataDiagnostic>& OutDiagnostics)
	{
		OutMetadata.Reset();
		OutDiagnostics.Reset();
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (SimulationTick == 0) return EModelResult::Invalid;
		if (EntitiesById.Num() > MaxEntities) return EModelResult::LimitReached;

		TArray<FSpatialEntityMetadata> Candidate;
		Candidate.Reserve(EntitiesById.Num());
		bool bHadDiagnostics = false;
		auto AddDiagnostic = [&OutDiagnostics, &bHadDiagnostics](const FNetworkEntityIdentity& Identity,
		                                                         ESpatialMetadataDiagnosticCode Code)
		{
			bHadDiagnostics = true;
			if (OutDiagnostics.Num() < MaxSpatialMetadataDiagnostics - 1)
			{
				FSpatialMetadataDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
				Diagnostic.Identity = Identity;
				Diagnostic.Code = Code;
				return;
			}
			if (OutDiagnostics.Num() == MaxSpatialMetadataDiagnostics - 1)
			{
				FSpatialMetadataDiagnostic& Summary = OutDiagnostics.AddDefaulted_GetRef();
				Summary.Code = ESpatialMetadataDiagnosticCode::DiagnosticsTruncated;
				Summary.OmittedDiagnosticCount = 1;
				return;
			}
			++OutDiagnostics.Last().OmittedDiagnosticCount;
		};

		TArray<uint32> SortedIds;
		EntitiesById.GenerateKeyArray(SortedIds);
		SortedIds.Sort();
		for (uint32 EntityId : SortedIds)
		{
			const FEntityRecord& Entity = EntitiesById.FindChecked(EntityId);
			const FSpatialDefinitionRegistration* Registration = SpatialDefinitions.Find(
				Entity.Identity.DefinitionStableId);
			if (!Registration)
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::MissingDefinitionPolicy);
				continue;
			}
			if (Registration->Policy == ESpatialDefinitionPolicy::SkipWithoutPosition)
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::NonSpatialSkipped);
				continue;
			}

			FSpatialEntityMetadata Metadata;
			Metadata.Identity = Entity.Identity;
			Metadata.OwnerConnectionToken = Entity.OwnerConnectionToken;
			Metadata.Revision = Entity.StateRevision;
			const FlecsModRegistry::FEntityDefinition* Definition = Registry.FindEntityDefinition(
				Entity.Identity.DefinitionStableId);
			if (!Definition || Definition->ComponentIds.Num() > MaxComponentsPerEntity)
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::UnknownComponentSchema);
				continue;
			}
			TArray<FString> AllowedComponents = Definition->ComponentIds;
			AllowedComponents.Sort();
			uint64 EstimatedBytes = EstimatedReplicationRecordHeaderBytes; // entity-create record
			bool bUnknownAllowedComponentSchema = false;
			for (const FString& StableComponentId : AllowedComponents)
			{
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
				if (!Schema || !IsValidVisibility(Schema->Visibility))
				{
					bUnknownAllowedComponentSchema = true;
					break;
				}
				// Count one potential upsert and one potential removal for every
				// component the definition can add during its lifetime.
				const uint64 SchemaTransitionBytes = 2ull *
					(EstimatedReplicationRecordHeaderBytes + static_cast<uint64>(Schema->MaxEncodedBytes));
				if (SchemaTransitionBytes > MAX_uint64 - EstimatedBytes)
				{
					EstimatedBytes = MAX_uint64;
					break;
				}
				EstimatedBytes += SchemaTransitionBytes;
			}
			if (bUnknownAllowedComponentSchema)
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::UnknownComponentSchema);
				continue;
			}
			if (Registration->Policy == ESpatialDefinitionPolicy::AlwaysRelevantWithoutPosition)
			{
				Metadata.bAlwaysRelevant = true;
			}
			else if (!Registration->PositionAccessor(Entity.LocalEntityToken, Metadata.Position))
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::PositionAccessorFailed);
				continue;
			}
			else if (Metadata.Position.ContainsNaN())
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::InvalidPosition);
				continue;
			}

			bool bHasImportance = false;
			bool bHasRecommendedRate = false;
			bool bUnknownComponentSchema = false;
			for (const FString& StableComponentId : Entity.PresentComponents)
			{
				const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
				if (!Schema || !IsValidVisibility(Schema->Visibility))
				{
					bUnknownComponentSchema = true;
					break;
				}
				if (!bHasImportance || Schema->Priority > Metadata.Importance)
				{
					Metadata.Importance = Schema->Priority;
					bHasImportance = true;
				}
				Metadata.bAlwaysRelevant |= Schema->Visibility == FlecsModRegistry::EVisibilityPolicy::Always;
				if (Schema->UpdateRateHz > 0.0f &&
					(!bHasRecommendedRate || Schema->UpdateRateHz < Metadata.RecommendedUpdateRateHz))
				{
					Metadata.RecommendedUpdateRateHz = Schema->UpdateRateHz;
					bHasRecommendedRate = true;
				}
			}
			if (bUnknownComponentSchema)
			{
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::UnknownComponentSchema);
				continue;
			}
			Metadata.EstimatedUpdateBytes = static_cast<uint32>(FMath::Min<uint64>(EstimatedBytes, MAX_uint32));
			if (EstimatedBytes > MaxBatchBytes)
				AddDiagnostic(Entity.Identity, ESpatialMetadataDiagnosticCode::EstimatedUpdateExceedsTransferCap);
			Metadata.RecommendedUpdateRateHz = FMath::Clamp(Metadata.RecommendedUpdateRateHz, 0.0f, 60.0f);
			Candidate.Add(MoveTemp(Metadata));
		}
		OutMetadata = MoveTemp(Candidate);
		OutDiagnostics.Shrink();
		return bHadDiagnostics ? EModelResult::Partial : EModelResult::Accepted;
	}

	EModelResult FWorkerEntityModel::BuildLiveEntityIdentities(TArray<FNetworkEntityIdentity>& OutIdentities) const
	{
		OutIdentities.Reset();
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (EntitiesById.Num() > MaxEntities) return EModelResult::LimitReached;

		TArray<uint32> SortedIds;
		EntitiesById.GenerateKeyArray(SortedIds);
		SortedIds.Sort();
		TArray<FNetworkEntityIdentity> Candidate;
		Candidate.Reserve(SortedIds.Num());
		for (uint32 EntityId : SortedIds)
		{
			const FEntityRecord* Entity = EntitiesById.Find(EntityId);
			if (!Entity || !Entity->Identity.IsValid() || Entity->Identity.NetworkEntityId != EntityId ||
				Entity->Identity.ServerWorldEpoch != ServerWorldEpoch)
				return EModelResult::StaleIdentity;
			Candidate.Add(Entity->Identity);
		}
		OutIdentities = MoveTemp(Candidate);
		return EModelResult::Accepted;
	}

#if WITH_DEV_AUTOMATION_TESTS
	EModelResult FWorkerEntityModel::InspectComponentBytesForTesting(const FNetworkEntityIdentity& Identity,
	                                                                 const FString& StableComponentId,
	                                                                 TArray<uint8>& OutBytes)
	{
		OutBytes.Reset();
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (bAuthoritative ? !bAuthoritativeSessionUsable : !bClientSessionUsable)
			return EModelResult::SessionInvalidated;
		FEntityRecord* Record = nullptr;
		if (!ResolveIdentity(Identity, Record)) return EModelResult::StaleIdentity;
		if (!Record->PresentComponents.Contains(StableComponentId)) return EModelResult::Invalid;
		if (!IsComponentAllowed(Record->Identity.DefinitionStableId, StableComponentId)) return
			EModelResult::UnknownSchema;
		const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
		if (!Schema || !Schema->Extract || !IsValidVisibility(Schema->Visibility)) return EModelResult::UnknownSchema;
		TArray<uint8> Candidate;
		if (!Schema->Extract(Record->LocalEntityToken, Candidate)) return EModelResult::CallbackFailed;
		if (static_cast<uint32>(Candidate.Num()) > Schema->MaxEncodedBytes ||
			static_cast<uint32>(Candidate.Num()) > MaxPayloadBytesPerRecord)
			return EModelResult::LimitReached;
		OutBytes = MoveTemp(Candidate);
		return EModelResult::Accepted;
	}
#endif

	EModelResult FWorkerEntityModel::ExecuteAuthoritativeCommand(const FClientCommand& Command,
	                                                             uint32 CurrentSimulationTick,
	                                                             const FlecsModRegistry::FSessionMapping&
	                                                             PeerWireMapping)
	{
		if (!IsWorkerThread()) return EModelResult::WrongRole;
		if (!bAuthoritative) return EModelResult::WrongRole;
		if (!bAuthoritativeSessionUsable) return EModelResult::SessionInvalidated;
		if (Command.ServerWorldEpoch != ServerWorldEpoch) return EModelResult::WrongEpoch;
		if (Command.TrustedConnectionToken == 0 || Command.Sequence == 0 || Command.TargetNetworkEntityId == 0 ||
			Command.TargetGeneration == 0 || Command.CommandWireId == 0 || Command.RequestedSimulationTick == 0 ||
			CurrentSimulationTick == 0 ||
			Command.Payload.Num() < 0 || static_cast<uint32>(Command.Payload.Num()) >
			FlecsModRegistry::MaxCommandPayloadBytes)
			return EModelResult::Invalid;
		if (!PeerWireMapping.IsValid() || (Command.RequestedSimulationTick > CurrentSimulationTick &&
				Command.RequestedSimulationTick - CurrentSimulationTick > MaxFutureCommandTicks) ||
			(Command.RequestedSimulationTick < CurrentSimulationTick &&
				CurrentSimulationTick - Command.RequestedSimulationTick > MaxPastCommandTicks))
			return EModelResult::StaleTick;

		FEntityRecord* Target = EntitiesById.Find(Command.TargetNetworkEntityId);
		if (!Target || Target->Identity.Generation != Command.TargetGeneration || Target->Identity.ServerWorldEpoch !=
			Command.ServerWorldEpoch)
			return EModelResult::StaleIdentity;
		FlecsModRegistry::FSessionWireEntry CommandEntry;
		if (!PeerWireMapping.FindEntryByWireId(Command.CommandWireId, CommandEntry) || CommandEntry.Kind !=
			FlecsModRegistry::EEntryKind::Command)
			return EModelResult::UnknownSchema;
		const FlecsModRegistry::FCommandSchema* Schema = Registry.FindCommand(CommandEntry.StableId);
		if (!Schema || !Schema->Execute || static_cast<uint32>(Command.Payload.Num()) > Schema->MaxPayloadBytes)
			return EModelResult::UnknownSchema;
		if (Target->StateRevision == MAX_uint64) return EModelResult::LimitReached;

		if (!CommandWindows.Contains(Command.TrustedConnectionToken) && CommandWindows.Num() >= 4096)
			return EModelResult::LimitReached;
		FCommandWindow& Window = CommandWindows.FindOrAdd(Command.TrustedConnectionToken);
		uint32& LastSequence = Command.bDiscrete ? Window.LastDiscreteSequence : Window.LastContinuousSequence;
		const uint32 SequenceDistance = Command.Sequence - LastSequence;
		if (SequenceDistance == 0 || SequenceDistance > 1024) return EModelResult::Replay;
		if (Window.WindowStartTick == 0 || CurrentSimulationTick - Window.WindowStartTick >= CommandWindowTicks)
		{
			Window.WindowStartTick = CurrentSimulationTick;
			Window.CommandsInWindow = 0;
		}
		if (Window.CommandsInWindow >= MaxCommandsPerConnectionPerWindow) return EModelResult::RateLimited;
		LastSequence = Command.Sequence;
		++Window.CommandsInWindow;
		if (!PermissionCheck(Command.TrustedConnectionToken, Schema->RequiredPermission,
		                     Target->OwnerConnectionToken, Command.TargetNetworkEntityId, Command.TargetGeneration))
			return EModelResult::Unauthorized;
		if (Schema->Validate && !Schema->Validate(Command.TrustedConnectionToken, Target->LocalEntityToken,
		                                          Command.Payload))
			return EModelResult::Invalid;
		if (!Schema->Execute(Command.TrustedConnectionToken, Target->LocalEntityToken, Command.Payload))
		{
			bAuthoritativeSessionUsable = false;
			return EModelResult::SessionInvalidated;
		}
		++Target->StateRevision;
		Target->LastEntityTick = FMath::Max(Target->LastEntityTick, CurrentSimulationTick);
		return EModelResult::Accepted;
	}

	bool FWorkerEntityModel::IsWorkerThread() const
	{
		return bInitialized && OwnerThreadId == FPlatformTLS::GetCurrentThreadId() && WorkerWorld != nullptr;
	}

	bool FWorkerEntityModel::ResolveIdentity(const FNetworkEntityIdentity& Identity, FEntityRecord*& OutRecord)
	{
		OutRecord = nullptr;
		if (!Identity.IsValid() || Identity.ServerWorldEpoch != ServerWorldEpoch) return false;
		FEntityRecord* Record = EntitiesById.Find(Identity.NetworkEntityId);
		if (!Record || Record->Identity.Generation != Identity.Generation ||
			Record->Identity.DefinitionStableId != Identity.DefinitionStableId)
			return false;
		OutRecord = Record;
		return true;
	}

	bool FWorkerEntityModel::IsComponentAllowed(const FString& DefinitionStableId,
	                                            const FString& ComponentStableId) const
	{
		const FlecsModRegistry::FEntityDefinition* Definition = Registry.FindEntityDefinition(DefinitionStableId);
		const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(ComponentStableId);
		return Definition && Schema && Definition->ComponentIds.Contains(ComponentStableId) &&
		(Schema->AllowedEntityDefinitions.IsEmpty() || Schema->AllowedEntityDefinitions.
		                                                       Contains(DefinitionStableId));
	}

	void FWorkerEntityModel::RemoveEntity(uint32 NetworkEntityId)
	{
		FEntityRecord* Record = EntitiesById.Find(NetworkEntityId);
		if (!Record) return;
		// Mod component callbacks may own worker-confined side state keyed by the
		// local entity token. Release every tracked component before Flecs destroys it.
		for (const FString& StableComponentId : Record->PresentComponents)
		{
			const FlecsModRegistry::FComponentSchema* Schema = Registry.FindComponent(StableComponentId);
			if (Schema && Schema->Remove) Schema->Remove(Record->LocalEntityToken);
		}
		if (Record->LocalEntityToken != 0 && WorkerWorld->is_alive(Record->LocalEntityToken))
			flecs::entity(WorkerWorld->c_ptr(), Record->LocalEntityToken).destruct();
		EntitiesById.Remove(NetworkEntityId);
	}

	void FWorkerEntityModel::RetireEntityId(uint32 NetworkEntityId)
	{
		uint16& NextGeneration = NextGenerationById.FindOrAdd(NetworkEntityId);
		if (NextGeneration == 0) NextGeneration = 1;
		RetiredEntityIds.Add(NetworkEntityId);
		if (NextGeneration == MAX_uint16) return;
		++NextGeneration;
		ReusableEntityIds.AddUnique(NetworkEntityId);
	}
}
