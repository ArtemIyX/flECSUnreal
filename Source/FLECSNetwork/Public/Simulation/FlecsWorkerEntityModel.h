#pragma once

#include "CoreMinimal.h"
#include "ModRegistry/ModSchemaRegistry.h"

namespace flecs
{
	struct world;
}

namespace FlecsSimulation
{
	constexpr int32 MaxEntities = 5000;
	constexpr int32 MaxIdentityHistory = 100000;
	constexpr int32 MaxComponentsPerEntity = 256;
	constexpr int32 MaxRecordsPerBatch = 16384;
	constexpr uint32 MaxBatchBytes = 4 * 1024 * 1024;
	constexpr int32 MaxSpatialMetadataDiagnostics = 128;
	constexpr int32 MaxPeerReplicationStates = 64;
	constexpr int32 MaxPeerKnownComponents = MaxRecordsPerBatch;
	constexpr int32 MaxCommandsPerConnectionPerWindow = 60;
	constexpr uint32 CommandWindowTicks = 60;
	constexpr uint32 MaxFutureCommandTicks = 30;
	constexpr uint32 MaxPastCommandTicks = 120;

	/** Server-assigned network identity; it never contains a local Flecs entity ID. */
	struct FNetworkEntityIdentity
	{
		uint64 ServerWorldEpoch = 0;
		uint32 NetworkEntityId = 0;
		uint16 Generation = 0;
		FString DefinitionStableId;

		bool IsValid() const
		{
			return ServerWorldEpoch != 0 && NetworkEntityId != 0 && Generation != 0 && !DefinitionStableId.IsEmpty();
		}
	};

	enum class EStateOperation : uint8
	{
		EntityCreate,
		ComponentUpsert,
		ComponentRemove,
		EntityDespawn,
		EntityLeaveRelevance
	};

	/** Owned DTO record. Component payloads are versioned by the peer's session mapping. */
	struct FStateRecord
	{
		FNetworkEntityIdentity Identity;
		uint16 DefinitionWireId = 0;
		uint16 ComponentWireId = 0;
		uint32 SourceSimulationTick = 0;
		EStateOperation Operation = EStateOperation::EntityCreate;
		TArray<uint8> Payload;
	};

	struct FStateBatch
	{
		uint64 ServerWorldEpoch = 0;
		uint32 SimulationTick = 0;
		/** Worker-issued token used to commit or discard one per-peer delta baseline. */
		uint64 PublicationRevision = 0;
		bool bCompleteStateSnapshot = false;
		TArray<FStateRecord> Records;
	};

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

	/** Scheduler-selected entity update; empty component list means all visible components are due when the flag is true. */
	struct FSelectedEntityComponents
	{
		FNetworkEntityIdentity Identity;
		TArray<FString> DueComponentStableIds;
		bool bAllVisibleComponentsDue = true;
	};

	/** Copied scheduler input; contains no component payloads or Flecs handles. */
	struct FSpatialEntityMetadata
	{
		FNetworkEntityIdentity Identity;
		FVector3d Position = FVector3d::ZeroVector;
		/** Monotonic worker-owned entity state revision, independent of the global fixed-step tick. */
		uint64 Revision = 0;
		uint64 OwnerConnectionToken = 0;
		int32 Importance = 0;
		uint32 EstimatedUpdateBytes = 0;
		float RecommendedUpdateRateHz = 0.0f;
		bool bAlwaysRelevant = false;
	};

	enum class ESpatialDefinitionPolicy : uint8
	{
		PositionRequired,
		AlwaysRelevantWithoutPosition,
		SkipWithoutPosition
	};

	enum class ESpatialMetadataDiagnosticCode : uint8
	{
		MissingDefinitionPolicy,
		NonSpatialSkipped,
		PositionAccessorFailed,
		InvalidPosition,
		UnknownComponentSchema,
		EstimatedUpdateExceedsTransferCap,
		DiagnosticsTruncated
	};

	struct FSpatialMetadataDiagnostic
	{
		FNetworkEntityIdentity Identity;
		ESpatialMetadataDiagnosticCode Code = ESpatialMetadataDiagnosticCode::MissingDefinitionPolicy;
		/** Nonzero only for the final bounded truncation summary entry. */
		uint32 OmittedDiagnosticCount = 0;
	};

	struct FInitialComponent
	{
		FString StableComponentId;
		TArray<uint8> Payload;
	};

	struct FClientCommand
	{
		uint64 ServerWorldEpoch = 0;
		uint64 TrustedConnectionToken = 0; // assigned from the owning server endpoint, never copied from client bytes
		uint32 Sequence = 0;
		uint32 TargetNetworkEntityId = 0;
		uint16 TargetGeneration = 0;
		uint32 RequestedSimulationTick = 0;
		uint16 CommandWireId = 0;
		bool bDiscrete = true;
		TArray<uint8> Payload;
	};

	enum class EModelResult : uint8
	{
		Accepted, Partial, Invalid, WrongRole, WrongEpoch, StaleIdentity, StaleTick,
		UnknownSchema, Unauthorized, RateLimited, Replay, LimitReached, CallbackFailed,
		/** A callback left mutation state uncertain; the bridge must discard/reset this session. */
		SessionInvalidated
	};

	/**
	 * Mutable ECS identity/state model. Construct, initialize, and call every method
	 * from the owning simulation worker only. Flecs entity handles remain private to
	 * this object. The local attachment epoch is deliberately not stored here: the
	 * server world epoch is a distinct network-session value.
	 */
	class FLECSNETWORK_API FWorkerEntityModel
	{
	public:
		using FPermissionCheck = TFunction<bool(uint64 ConnectionToken, const FString& Permission,
		                                        uint64 CurrentOwnerConnectionToken, uint32 TargetNetworkEntityId,
		                                        uint16 TargetGeneration)>;
		/**
		 * Trusted native worker callback installed before simulation starts. EntityToken is
		 * opaque, valid only inside this callback on the owning worker, and never exported.
		 */
		using FPositionAccessor = TFunction<bool(uint64 EntityToken, FVector3d& OutPosition)>;

		FWorkerEntityModel(FlecsModRegistry::FRegistry& InRegistry, bool bInAuthoritative);
		~FWorkerEntityModel();

		EModelResult Initialize(flecs::world& WorkerWorld, uint64 InServerWorldEpoch,
		                        FPermissionCheck InPermissionCheck = FPermissionCheck());
		EModelResult Shutdown();
		/** Register trusted spatial metadata policy before spawning this definition's entities. */
		EModelResult RegisterSpatialDefinition(const FString& DefinitionStableId,
		                                       ESpatialDefinitionPolicy Policy,
		                                       FPositionAccessor PositionAccessor = FPositionAccessor());
		EModelResult SpawnAuthoritative(const FString& DefinitionStableId, uint64 OwnerConnectionToken,
		                                TConstArrayView<FInitialComponent> InitialComponents,
		                                FNetworkEntityIdentity& OutIdentity);
		EModelResult SetOwnerAuthoritative(const FNetworkEntityIdentity& Identity, uint64 NewOwnerConnectionToken);
		/** Add an allowlisted replicated component on the owning worker. */
		EModelResult AddComponentAuthoritative(const FNetworkEntityIdentity& Identity, const FString& StableComponentId,
		                                       TConstArrayView<uint8> EncodedPayload, uint32 SimulationTick);
		/** Replace an existing allowlisted component; lifecycle ticks must increase strictly. */
		EModelResult UpdateComponentAuthoritative(const FNetworkEntityIdentity& Identity,
		                                          const FString& StableComponentId,
		                                          TConstArrayView<uint8> EncodedPayload, uint32 SimulationTick);
		/** Remove an existing component; the per-component tick is retained for safe re-add. */
		EModelResult RemoveComponentAuthoritative(const FNetworkEntityIdentity& Identity,
		                                          const FString& StableComponentId,
		                                          uint32 SimulationTick);
		EModelResult DespawnAuthoritative(const FNetworkEntityIdentity& Identity, uint32 SimulationTick);
		/** Detect encoded changes made by worker systems to already registered components. */
		EModelResult RefreshAuthoritativeComponentChanges(uint32 SimulationTick);

		/** Apply a bounded received batch on the client worker at a fixed-step boundary. */
		EModelResult ApplyClientBatch(const FStateBatch& Batch,
		                              const FlecsModRegistry::FSessionMapping& PeerWireMapping);
		bool IsClientSessionUsable() const { return bClientSessionUsable; }
		/**
		 * Extract a complete copied snapshot for one trusted peer on the authoritative worker.
		 * The bridge must cap RelevantEntityKeys at MaxEntities and create a fresh context
		 * for each live connection incarnation. OwnerOnly components are included only for
		 * their current owner; the returned DTO contains only peer-mapped wire IDs.
		 */
		EModelResult BuildAuthoritativeSnapshot(uint32 SimulationTick,
		                                        const FPeerReplicationContext& PeerContext,
		                                        const FlecsModRegistry::FSessionMapping& PeerWireMapping,
		                                        FStateBatch& OutBatch);
		/** Build one bounded, ACK-paced per-peer delta; only one publication may be pending per peer. */
		EModelResult BuildAuthoritativeDelta(uint32 SimulationTick,
		                                     const FPeerReplicationContext& PeerContext,
		                                     const FlecsModRegistry::FSessionMapping& PeerWireMapping,
		                                     TConstArrayView<FSelectedEntityComponents> DueSelection,
		                                     FStateBatch& OutBatch);
		/** Commit pending per-peer payload/tick state after the transport confirms the full publication. */
		EModelResult AcknowledgeAuthoritativeDelta(uint64 ConnectionToken, uint64 InServerWorldEpoch,
		                                           uint64 PublicationRevision);
		/** Discard a failed pending publication; bForgetPeer also clears acknowledged state on connection close. */
		EModelResult DiscardAuthoritativeDelta(uint64 ConnectionToken, uint64 InServerWorldEpoch,
		                                       uint64 PublicationRevision, bool bForgetPeer = false);
		/**
		 * Build deterministic scheduler input on the authoritative worker. Valid entities
		 * are returned even when unsupported or invalid entities are skipped; Partial marks
		 * that case and bounded diagnostics explain each skip (with a truncation summary).
		 * Importance is the maximum present schema priority. Always relevance is true when
		 * the definition policy or any present schema declares Always. Estimated bytes charge
		 * create plus possible upsert and removal of every allowlisted schema, including
		 * owner-only state. Fast Array framing is excluded; estimates above 4 MiB are diagnosed.
		 * Recommended rate is the minimum positive present schema rate capped at 60 Hz, so
		 * an entity-level scheduler does not exceed any component's declared rate limit.
		 */
		EModelResult BuildSpatialMetadata(uint32 SimulationTick,
		                                  TArray<FSpatialEntityMetadata>& OutMetadata,
		                                  TArray<FSpatialMetadataDiagnostic>& OutDiagnostics);
		/** Complete bounded live identity set, independent of spatial accessor success. */
		EModelResult BuildLiveEntityIdentities(TArray<FNetworkEntityIdentity>& OutIdentities) const;
#if WITH_DEV_AUTOMATION_TESTS
		/** Worker-only test probe; returns bounded copied codec bytes without exposing the local entity token. */
		EModelResult InspectComponentBytesForTesting(const FNetworkEntityIdentity& Identity,
		                                             const FString& StableComponentId, TArray<uint8>& OutBytes);
#endif

		/** Validate and execute after the game thread has copied the complete bounded envelope. */
		EModelResult ExecuteAuthoritativeCommand(const FClientCommand& Command, uint32 CurrentSimulationTick,
		                                         const FlecsModRegistry::FSessionMapping& PeerWireMapping);

		uint64 GetServerWorldEpoch() const { return ServerWorldEpoch; }
		uint32 GetEntityCount() const { return static_cast<uint32>(EntitiesById.Num()); }

	private:
		struct FEntityRecord
		{
			FNetworkEntityIdentity Identity;
			uint64 OwnerConnectionToken = 0;
			/** Local Flecs token, private to this worker-owned object. Never serialized. */
			uint64 LocalEntityToken = 0;
			uint32 LastEntityTick = 0;
			uint64 StateRevision = 1;
			TSet<FString> PresentComponents;
			TMap<FString, uint32> LastComponentTicks;
			TMap<FString, TArray<uint8>> LastComponentPayloads;
		};

		/** Tick high-water data retained while a client replica is outside relevance. */
		struct FReplicaHistory
		{
			uint16 Generation = 0;
			FString DefinitionStableId;
			uint32 LastEntityTick = 0;
			TMap<FString, uint32> LastComponentTicks;
			TMap<FString, TArray<uint8>> LastComponentPayloads;
		};

		/** Bounded replay tombstone for an already-applied authoritative despawn. */
		struct FDespawnTombstone
		{
			uint64 ServerWorldEpoch = 0;
			uint16 Generation = 0;
			uint32 SimulationTick = 0;
			FString DefinitionStableId;
		};

		struct FCommandWindow
		{
			uint32 LastDiscreteSequence = 0;
			uint32 LastContinuousSequence = 0;
			uint32 WindowStartTick = 0;
			uint16 CommandsInWindow = 0;
		};

		struct FSpatialDefinitionRegistration
		{
			ESpatialDefinitionPolicy Policy = ESpatialDefinitionPolicy::PositionRequired;
			FPositionAccessor PositionAccessor;
		};

		struct FPeerComponentBaseline
		{
			TArray<uint8> Payload;
			uint32 SourceSimulationTick = 0;
		};

		struct FPeerEntityBaseline
		{
			FNetworkEntityIdentity Identity;
			TMap<FString, FPeerComponentBaseline> Components;
		};

		struct FPeerDeltaState
		{
			TMap<uint32, FPeerEntityBaseline> AcknowledgedEntities;
			TMap<uint32, FPeerEntityBaseline> PendingEntities;
			FStateBatch PendingBatch;
			uint64 AcknowledgedPayloadBytes = 0;
			uint64 PendingPayloadBytes = 0;
		};

		bool IsWorkerThread() const;
		bool ResolveIdentity(const FNetworkEntityIdentity& Identity, FEntityRecord*& OutRecord);
		EModelResult SetComponentAuthoritative(const FNetworkEntityIdentity& Identity, const FString& StableComponentId,
		                                       TConstArrayView<uint8> EncodedPayload, uint32 SimulationTick,
		                                       bool bRequireExisting);
		EModelResult CreateClientEntity(const FNetworkEntityIdentity& Identity, const FString& DefinitionStableId,
		                                FEntityRecord*& OutRecord);
		EModelResult ApplyRecord(const FStateRecord& Record, uint32 BatchTick,
		                         const FlecsModRegistry::FSessionMapping& PeerWireMapping);
		EModelResult ValidateBatch(const FStateBatch& Batch) const;
		void RemoveEntity(uint32 NetworkEntityId);
		void RetireEntityId(uint32 NetworkEntityId);
		bool IsComponentAllowed(const FString& DefinitionStableId, const FString& ComponentStableId) const;

		FlecsModRegistry::FRegistry& Registry;
		bool bAuthoritative = false;
		bool bInitialized = false;
		bool bClientSessionUsable = true;
		bool bAuthoritativeSessionUsable = true;
		uint64 ServerWorldEpoch = 0;
		uint32 LastAppliedBatchTick = 0;
		uint32 NextNetworkEntityId = 1;
		uint32 OwnerThreadId = 0;
		flecs::world* WorkerWorld = nullptr;
		FPermissionCheck PermissionCheck;
		TMap<FString, FSpatialDefinitionRegistration> SpatialDefinitions;
		TSet<FString> SpawnedDefinitionIds;
		TMap<uint32, FEntityRecord> EntitiesById;
		TMap<uint32, FReplicaHistory> ReplicaHistoryById;
		TMap<uint32, FDespawnTombstone> DespawnTombstonesById;
		TMap<uint64, FPeerDeltaState> PeerDeltaStates;
		uint64 NextPublicationRevision = 1;
		TMap<uint32, uint16> NextGenerationById;
		TSet<uint32> RetiredEntityIds;
		TArray<uint32> ReusableEntityIds;
		TMap<uint64, FCommandWindow> CommandWindows;
	};
}
