#include "Networking/FlecsGenericNetworkSessionSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Subsystems/FlecsGameInstanceSubsystem.h"
#include "FlecsReplicationChannel.h"
#include "Simulation/FlecsWorkerEntityModel.h"
#include "FlecsSimulationTypes.h"
#include "Relevance/FlecsRelevanceScheduler.h"
#include "Engine/NetConnection.h"
#include "Stats/Stats.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include <atomic>

namespace
{
	FFlecsNetworkSessionConfigurator DefaultSessionConfigurator;
	uint32 DefaultSessionProtocolVersion = 0;
	constexpr uint32 MaxTransportSchemaPayloadBytes = 1024;
	constexpr int32 MaxChannelUpdateRecords = 16384;
	constexpr uint64 MaxChannelUpdatePayloadBytes = 4 * 1024 * 1024;
	constexpr uint32 SpatialMetadataTypeId = 0x464c534d;
	constexpr uint32 LiveIdentityTypeId = 0x464c4944;
	constexpr uint32 SpatialPublicationCompleteTypeId = 0x464c5343;
	constexpr uint32 MaxPublishedLiveIdentities = 5000;
	constexpr int32 MaxRetirementBarriersQueuedPerTick = 16;
	constexpr int32 MaxPendingRetiredIncarnations = 1024;
	constexpr int32 MaxPeerOwnedEntities = 256;
	constexpr int32 SpatialMetadataPayloadFixedBytes = 55;
	uint64 NextServerWorldEpoch = 0;
	uint64 NextConnectionIncarnation = 0;

	void AppendU16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value >> 8));
		Bytes.Add(static_cast<uint8>(Value));
	}

	void AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value >> 24));
		Bytes.Add(static_cast<uint8>(Value >> 16));
		Bytes.Add(static_cast<uint8>(Value >> 8));
		Bytes.Add(static_cast<uint8>(Value));
	}

	void AppendU64(TArray<uint8>& Bytes, uint64 Value)
	{
		AppendU32(Bytes, static_cast<uint32>(Value >> 32));
		AppendU32(Bytes, static_cast<uint32>(Value));
	}

	uint16 ReadU16(TConstArrayView<uint8> Bytes, int32& Offset)
	{
		const uint16 Value = static_cast<uint16>((static_cast<uint16>(Bytes[Offset]) << 8) | Bytes[Offset + 1]);
		Offset += 2;
		return Value;
	}

	uint32 ReadU32(TConstArrayView<uint8> Bytes, int32& Offset)
	{
		const uint32 Value = (static_cast<uint32>(Bytes[Offset]) << 24) |
			(static_cast<uint32>(Bytes[Offset + 1]) << 16) |
			(static_cast<uint32>(Bytes[Offset + 2]) << 8) | Bytes[Offset + 3];
		Offset += 4;
		return Value;
	}

	uint64 ReadU64(TConstArrayView<uint8> Bytes, int32& Offset)
	{
		const uint32 High = ReadU32(Bytes, Offset);
		const uint32 Low = ReadU32(Bytes, Offset);
		return (static_cast<uint64>(High) << 32) | Low;
	}

	bool EncodeSpatialMetadata(const FlecsSimulation::FSpatialEntityMetadata& Source, TArray<uint8>& OutBytes)
	{
		if (!Source.Identity.IsValid() || !FlecsModRegistry::FRegistry::IsValidStableId(Source.Identity.DefinitionStableId) ||
			Source.Identity.DefinitionStableId.Len() > static_cast<int32>(FlecsModRegistry::MaxStableIdLength) ||
			!FMath::IsFinite(Source.Position.X) ||
			!FMath::IsFinite(Source.Position.Y) || !FMath::IsFinite(Source.Position.Z) ||
			Source.Position.X < MIN_int32 || Source.Position.X > MAX_int32 ||
			Source.Position.Y < MIN_int32 || Source.Position.Y > MAX_int32 ||
			Source.Position.Z < MIN_int32 || Source.Position.Z > MAX_int32 ||
			!FMath::IsFinite(Source.RecommendedUpdateRateHz) ||
			Source.RecommendedUpdateRateHz < 0.0f || Source.RecommendedUpdateRateHz > 60.0f)
		{
			return false;
		}
		OutBytes.Reset(SpatialMetadataPayloadFixedBytes + 1 + Source.Identity.DefinitionStableId.Len());
		AppendU64(OutBytes, Source.Identity.ServerWorldEpoch);
		AppendU32(OutBytes, Source.Identity.NetworkEntityId);
		AppendU16(OutBytes, Source.Identity.Generation);
		AppendU32(OutBytes, static_cast<uint32>(FMath::RoundToInt(Source.Position.X)));
		AppendU32(OutBytes, static_cast<uint32>(FMath::RoundToInt(Source.Position.Y)));
		AppendU32(OutBytes, static_cast<uint32>(FMath::RoundToInt(Source.Position.Z)));
		AppendU64(OutBytes, Source.Revision);
		AppendU64(OutBytes, Source.OwnerConnectionToken);
		AppendU32(OutBytes, static_cast<uint32>(Source.Importance));
		OutBytes.Add(Source.bAlwaysRelevant ? 1 : 0);
		AppendU32(OutBytes, Source.EstimatedUpdateBytes);
		uint32 RateBits = 0;
		FMemory::Memcpy(&RateBits, &Source.RecommendedUpdateRateHz, sizeof(RateBits));
		AppendU32(OutBytes, RateBits);
		OutBytes.Add(static_cast<uint8>(Source.Identity.DefinitionStableId.Len()));
		for (TCHAR Character : Source.Identity.DefinitionStableId)
		{
			if (Character > 0x7f) return false;
			OutBytes.Add(static_cast<uint8>(Character));
		}
		return OutBytes.Num() == SpatialMetadataPayloadFixedBytes + 1 + Source.Identity.DefinitionStableId.Len();
	}

	bool DecodeSpatialMetadata(TConstArrayView<uint8> Bytes, FFlecsSpatialEntity& OutEntity,
		FString& OutDefinitionStableId)
	{
		if (Bytes.Num() < SpatialMetadataPayloadFixedBytes + 1)
		{
			return false;
		}
		int32 Offset = 0;
		OutEntity.Key.WorldEpoch = ReadU64(Bytes, Offset);
		OutEntity.Key.EntityId = ReadU32(Bytes, Offset);
		OutEntity.Key.Generation = ReadU16(Bytes, Offset);
		const int32 X = static_cast<int32>(ReadU32(Bytes, Offset));
		const int32 Y = static_cast<int32>(ReadU32(Bytes, Offset));
		const int32 Z = static_cast<int32>(ReadU32(Bytes, Offset));
		OutEntity.Position = FVector3f(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
		OutEntity.Revision = ReadU64(Bytes, Offset);
		OutEntity.OwnerConnectionId = ReadU64(Bytes, Offset);
		OutEntity.Importance = static_cast<int32>(ReadU32(Bytes, Offset));
		const uint8 Flags = Bytes[Offset++];
		OutEntity.EstimatedUpdateBytes = ReadU32(Bytes, Offset);
		const uint32 RateBits = ReadU32(Bytes, Offset);
		FMemory::Memcpy(&OutEntity.RecommendedUpdateRateHz, &RateBits, sizeof(RateBits));
		const uint8 StableIdLength = Bytes[Offset++];
		if (StableIdLength == 0 || StableIdLength > FlecsModRegistry::MaxStableIdLength ||
			Offset + StableIdLength != Bytes.Num())
		{
			return false;
		}
		OutDefinitionStableId.Reset(StableIdLength);
		for (uint8 Index = 0; Index < StableIdLength; ++Index)
		{
			const uint8 Character = Bytes[Offset++];
			if (Character > 0x7f) return false;
			OutDefinitionStableId.AppendChar(static_cast<TCHAR>(Character));
		}
		OutEntity.bAlwaysRelevant = (Flags & 1) != 0;
		OutEntity.Visibility = EFlecsNetworkVisibility::RelevantToAll;
		return Offset == Bytes.Num() && OutEntity.Key.IsValid() && (Flags & ~1) == 0 &&
		FlecsModRegistry::FRegistry::IsValidStableId(OutDefinitionStableId) &&
		FMath::IsFinite(OutEntity.RecommendedUpdateRateHz) && OutEntity.RecommendedUpdateRateHz >= 0.0f &&
		OutEntity.RecommendedUpdateRateHz <= 60.0f;
	}

	bool EncodeLiveIdentities(uint64 WorldEpoch, uint32 SimulationTick, const TArray<FlecsSimulation::FNetworkEntityIdentity>& Identities,
		TArray<uint8>& OutBytes)
	{
		if (WorldEpoch == 0 || SimulationTick == 0 || Identities.Num() > static_cast<int32>(MaxPublishedLiveIdentities)) return false;
		OutBytes.Reset(16 + Identities.Num() * 6);
		AppendU64(OutBytes, WorldEpoch);
		AppendU32(OutBytes, SimulationTick);
		AppendU32(OutBytes, static_cast<uint32>(Identities.Num()));
		TSet<uint32> SeenEntityIds;
		SeenEntityIds.Reserve(Identities.Num());
		for (const FlecsSimulation::FNetworkEntityIdentity& Identity : Identities)
		{
			if (!Identity.IsValid() || Identity.ServerWorldEpoch != WorldEpoch || SeenEntityIds.Contains(Identity.NetworkEntityId))
			{
				return false;
			}
			SeenEntityIds.Add(Identity.NetworkEntityId);
			AppendU32(OutBytes, Identity.NetworkEntityId);
			AppendU16(OutBytes, Identity.Generation);
		}
		return true;
	}

	bool DecodeLiveIdentities(TConstArrayView<uint8> Bytes, uint64 ExpectedWorldEpoch,
		uint32& OutSimulationTick, TArray<FFlecsNetworkEntityKey>& OutKeys)
	{
		OutSimulationTick = 0;
		OutKeys.Reset();
		if (Bytes.Num() < 16) return false;
		int32 Offset = 0;
		const uint64 WorldEpoch = ReadU64(Bytes, Offset);
		const uint32 SimulationTick = ReadU32(Bytes, Offset);
		const uint32 Count = ReadU32(Bytes, Offset);
		if (WorldEpoch == 0 || WorldEpoch != ExpectedWorldEpoch || SimulationTick == 0 || Count > MaxPublishedLiveIdentities ||
			Bytes.Num() != 16 + static_cast<int64>(Count) * 6)
		{
			return false;
		}
		OutKeys.Reserve(static_cast<int32>(Count));
		TSet<uint32> SeenEntityIds;
		SeenEntityIds.Reserve(static_cast<int32>(Count));
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FFlecsNetworkEntityKey Key;
			Key.WorldEpoch = WorldEpoch;
			Key.EntityId = ReadU32(Bytes, Offset);
			Key.Generation = ReadU16(Bytes, Offset);
			if (!Key.IsValid() || SeenEntityIds.Contains(Key.EntityId))
			{
				OutKeys.Reset();
				return false;
			}
			SeenEntityIds.Add(Key.EntityId);
			OutKeys.Add(Key);
		}
		OutSimulationTick = SimulationTick;
		return Offset == Bytes.Num();
	}

	bool ValidateManifestTransportLimits(const FlecsModRegistry::FManifestDescriptor& Descriptor, FString& OutError)
	{
		for (const FlecsModRegistry::FManifestEntry& Entry : Descriptor.Entries)
		{
			if ((Entry.Kind == FlecsModRegistry::EEntryKind::Component || Entry.Kind == FlecsModRegistry::EEntryKind::Command) &&
				Entry.MaxPayloadBytes > MaxTransportSchemaPayloadBytes)
			{
				OutError = FString::Printf(TEXT("%s '%s' declares %u payload bytes; the current Iris channel supports at most %u."),
					Entry.Kind == FlecsModRegistry::EEntryKind::Component ? TEXT("Component") : TEXT("Command"),
					*Entry.StableId, Entry.MaxPayloadBytes, MaxTransportSchemaPayloadBytes);
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	uint8 ToCommandExecutionResult(FlecsSimulation::EModelResult Result)
	{
		switch (Result)
		{
		case FlecsSimulation::EModelResult::Accepted:
			return FlecsNetworkCommandResult::ExecutionAccepted;
		case FlecsSimulation::EModelResult::Unauthorized:
			return FlecsNetworkCommandResult::ExecutionRejectedUnauthorized;
		case FlecsSimulation::EModelResult::WrongEpoch:
		case FlecsSimulation::EModelResult::StaleIdentity:
		case FlecsSimulation::EModelResult::StaleTick:
		case FlecsSimulation::EModelResult::Replay:
			return FlecsNetworkCommandResult::ExecutionRejectedStale;
		case FlecsSimulation::EModelResult::UnknownSchema:
			return FlecsNetworkCommandResult::ExecutionRejectedUnknownSchema;
		default:
			return FlecsNetworkCommandResult::ExecutionFailed;
		}
	}
}

struct FFlecsGenericPeerSchedulingRuntime
{
	FFlecsPerConnectionScheduler Scheduler;
	TSet<uint64> SelectedEntityKeys;
	TSet<uint64> PendingEntityKeys;
	FFlecsRelevanceSelection PendingSelection;
	TArray<FFlecsReplicationRecord> PendingWireRecords;
	uint32 PendingSimulationTick = 0;
	uint64 PendingModelRevision = 0;
	uint8 TransferRetryCount = 0;
	bool bSnapshotRequestPending = false;
	bool bTransferPending = false;
	bool bModelAckPending = false;
	bool bModelAckQueued = false;
	bool bTransferSucceeded = false;
	bool bPeerSpawnQueued = false;
	bool bSpawnPending = false;
	bool bPendingInitialSpawn = false;
	uint64 PendingSpawnRequestId = 0;
};

struct FFlecsGenericWorkerBridgeState
{
	struct FCompletedDelta
	{
		uint64 LocalAttachmentEpoch = 0;
		uint64 ServerWorldEpoch = 0;
		uint64 ConnectionIncarnation = 0;
		uint64 SelectionId = 0;
		uint64 ModelPublicationRevision = 0;
		FlecsSimulation::FStateBatch Batch;
		FlecsSimulation::EModelResult Result = FlecsSimulation::EModelResult::Invalid;
	};
	struct FClientApplyResult
	{
		uint64 LocalAttachmentEpoch = 0;
		uint64 ServerWorldEpoch = 0;
		uint64 ConnectionIncarnation = 0;
		uint64 SnapshotRevision = 0;
		uint32 SimulationTick = 0;
		FlecsSimulation::EModelResult Result = FlecsSimulation::EModelResult::Invalid;
#if WITH_DEV_AUTOMATION_TESTS
		FlecsSimulation::FNetworkEntityIdentity InspectedIdentity;
		TArray<uint8> InspectedBytes;
#endif
	};
	struct FSpawnResult
	{
		uint64 LocalAttachmentEpoch = 0;
		uint64 ServerWorldEpoch = 0;
		uint64 RequestId = 0;
		FlecsSimulation::FNetworkEntityIdentity Identity;
		FlecsSimulation::EModelResult Result = FlecsSimulation::EModelResult::Invalid;
	};
	struct FDeltaAckResult
	{
		uint64 LocalAttachmentEpoch = 0;
		uint64 ServerWorldEpoch = 0;
		uint64 ModelPublicationRevision = 0;
		uint64 SelectionId = 0;
		FlecsSimulation::EModelResult Result = FlecsSimulation::EModelResult::Invalid;
	};
	struct FCommandExecutionResult
	{
		uint64 ServerWorldEpoch = 0;
		uint32 Sequence = 0;
		FlecsSimulation::EModelResult Result = FlecsSimulation::EModelResult::Invalid;
	};

	FCriticalSection ResultMutex;
	std::atomic<bool> bLifecycleRetired{false}; // set on the game thread, read by worker callbacks
	std::atomic<bool> bAuthoritativeModelFailed{false}; // worker signals an unrecoverable model failure
	TMap<uint64, FCompletedDelta> CompletedDeltas;
	TMap<uint64, FClientApplyResult> ClientApplyResults;
	TMap<uint64, FSpawnResult> SpawnResults;
	TMap<uint64, FDeltaAckResult> DeltaAckResults;
	TMap<uint64, TMap<uint32, FCommandExecutionResult>> CommandExecutionResults;
	TSet<uint64> RetiredConnectionIncarnations; // guarded by ResultMutex
	bool bRetirementFailClosed = false; // guarded by ResultMutex; reset only with worker lifecycle replacement
	TMap<uint64, uint64> RetiredServerWorldEpochs; // guarded by ResultMutex
	TSet<uint64> RetirementBarrierQueued; // guarded by ResultMutex
	TSet<uint64> RetirementBarrierPassed; // guarded by ResultMutex
	TSet<uint64> CommandResultOverflowIncarnations; // guarded by ResultMutex
	TUniquePtr<FlecsSimulation::FWorkerEntityModel> Model; // worker thread only
	TMap<uint64, TArray<FlecsSimulation::FNetworkEntityIdentity>> OwnedEntitiesByConnection; // worker thread only
	uint64 LastClientSnapshotRevision = 0; // worker thread only
	uint64 ClientModelIncarnation = 0; // worker thread only
	uint64 LocalAttachmentEpoch = 0; // worker thread only
	uint64 ServerWorldEpoch = 0; // worker thread only
	uint32 SimulationTick = 0; // worker thread only
	bool bModelInitialized = false; // worker thread only
	bool bSpatialDiagnosticsReported = false; // worker thread only
};

bool UFlecsNetworkSessionSubsystem::SetDefaultConfigurator(FFlecsNetworkSessionConfigurator Configurator, uint32 ProtocolVersion)
{
	check(IsInGameThread());
	if (!Configurator || ProtocolVersion == 0) return false;
	DefaultSessionConfigurator = MoveTemp(Configurator);
	DefaultSessionProtocolVersion = ProtocolVersion;
	return true;
}

void UFlecsNetworkSessionSubsystem::ClearDefaultConfigurator()
{
	check(IsInGameThread());
	DefaultSessionConfigurator = {};
	DefaultSessionProtocolVersion = 0;
}

bool UFlecsNetworkSessionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return Super::ShouldCreateSubsystem(Outer) && DefaultSessionConfigurator && DefaultSessionProtocolVersion != 0;
}

void UFlecsNetworkSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	check(IsInGameThread());
	if (UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		FlecsGameInstanceSubsystem = GameInstance->GetSubsystem<UFlecsGameInstanceSubsystem>();
		if (UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get())
		{
			WorldDetachHandle = FlecsSubsystem->OnWorldAboutToDetach.AddUObject(
				this, &UFlecsNetworkSessionSubsystem::HandleWorldAboutToDetach);
		}
	}
	WorkerBridgeState = MakeShared<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe>();
	SessionProtocolVersion = DefaultSessionProtocolVersion;
	FString ModRegistrationError;
	const auto PermissionRegistrar = [this](const FString& Id, FFlecsTrustedPermissionRule Rule, FString& Error)
	{
		return RegisterTrustedPermissionRule(Id, MoveTemp(Rule), Error);
	};
	const auto SpatialRegistrar = [this](const FString& Id, EFlecsTrustedSpatialPolicy Policy,
		FFlecsTrustedPositionAccessor Accessor, FString& Error)
	{
		return RegisterTrustedSpatialDefinition(Id, Policy, MoveTemp(Accessor), Error);
	};
	const auto SpawnRegistrar = [this](FFlecsPeerSpawnFactory Factory, FString& Error)
	{
		return SetPeerSpawnFactory(MoveTemp(Factory), Error);
	};
	FFlecsNetworkSessionRegistrar Registrar{ModRegistry, PermissionRegistrar, SpatialRegistrar, SpawnRegistrar};
	Registrar.SetPeerInterestProvider = [this](FFlecsPeerInterestProvider Provider, FString& Error)
	{
		return SetPeerInterestProvider(MoveTemp(Provider), Error);
	};
	if (!DefaultSessionConfigurator || !GetWorld() ||
		!DefaultSessionConfigurator(*GetWorld(), Registrar, ModRegistrationError))
	{
		bModRegistrySetupFailed = true;
		UE_LOG(LogTemp, Error, TEXT("Failed to configure the Flecs network session: %s"), *ModRegistrationError);
	}
	if (UWorld* World = GetWorld(); World &&
		(World->GetNetMode() == NM_ListenServer || World->GetNetMode() == NM_DedicatedServer))
	{
		ServerWorldEpoch = ++NextServerWorldEpoch;
		if (ServerWorldEpoch == 0)
		{
			ServerWorldEpoch = ++NextServerWorldEpoch;
		}
	}
	ReconcileConnections();
}

void UFlecsNetworkSessionSubsystem::Deinitialize()
{
	check(IsInGameThread());
	bIsClosing = true;
	if (WorkerBridgeState.IsValid())
	{
		WorkerBridgeState->bLifecycleRetired.store(true, std::memory_order_release);
	}
	if (UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get())
	{
		if (WorldDetachHandle.IsValid())
		{
			FlecsSubsystem->OnWorldAboutToDetach.Remove(WorldDetachHandle);
			WorldDetachHandle.Reset();
		}
	}
	CloseAllSessions(GetWorld() && GetWorld()->GetNetMode() != NM_Client);
	FlecsGameInstanceSubsystem.Reset();
	Super::Deinitialize();
}

void UFlecsNetworkSessionSubsystem::Tick(float DeltaTime)
{
	check(IsInGameThread());
	if (!bModRegistryFrozen && !bModRegistrySetupFailed)
	{
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_Client)
		{
			UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
			if (!FlecsSubsystem || FlecsSubsystem->GetActiveEpoch() == 0)
			{
				return;
			}
		}
		FString FreezeError;
		if (!FreezeModRegistry(SessionProtocolVersion, FreezeError))
		{
			bModRegistrySetupFailed = true;
			UE_LOG(LogTemp, Error, TEXT("Failed to freeze the network mod registry: %s"), *FreezeError);
			CloseAllSessions(GetWorld() && GetWorld()->GetNetMode() != NM_Client);
			return;
		}
	}
	if (bModRegistrySetupFailed)
	{
		return;
	}
	ReconcileConnections();
	ProcessRequestedChannelRecreations();
	QueueRetirementBarriers();
	RetryPendingClientModelReset();
	ProcessServerCommandResults();
	ProcessServerReplication();
	ProcessClientReplication();
}


void UFlecsNetworkSessionSubsystem::QueueRetirementBarriers()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (!World || World->GetNetMode() == NM_Client || !FlecsSubsystem || !WorkerBridgeState.IsValid()) return;
	if (bRetirementFailClosed) CloseAllSessions(true);
	const uint64 ExpectedLocalEpoch = FlecsSubsystem->GetActiveEpoch();
	if (ExpectedLocalEpoch == 0 || ExpectedLocalEpoch != LocalAttachmentEpoch) return;

	TArray<TPair<uint64, uint64>> RetirementsToQueue;
	{
		FScopeLock Lock(&WorkerBridgeState->ResultMutex);
		for (const TPair<uint64, uint64>& Pair : WorkerBridgeState->RetiredServerWorldEpochs)
		{
			if (!WorkerBridgeState->RetirementBarrierQueued.Contains(Pair.Key) &&
				!WorkerBridgeState->RetirementBarrierPassed.Contains(Pair.Key))
			{
				WorkerBridgeState->RetirementBarrierQueued.Add(Pair.Key);
				RetirementsToQueue.Emplace(Pair.Key, Pair.Value);
				if (RetirementsToQueue.Num() >= MaxRetirementBarriersQueuedPerTick) break;
			}
		}
	}

	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	for (const TPair<uint64, uint64>& Retirement : RetirementsToQueue)
	{
		const uint64 Incarnation = Retirement.Key;
		const uint64 RetiredServerEpoch = Retirement.Value;
		const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(ExpectedLocalEpoch,
			[BridgeRef, ExpectedLocalEpoch, Incarnation, RetiredServerEpoch](flecs::world&)
			{
				check(!IsInGameThread());
				FScopeLock Lock(&BridgeRef->ResultMutex);
				const uint64* CurrentServerEpoch = BridgeRef->RetiredServerWorldEpochs.Find(Incarnation);
				if (BridgeRef->LocalAttachmentEpoch == ExpectedLocalEpoch && CurrentServerEpoch &&
					*CurrentServerEpoch == RetiredServerEpoch &&
					BridgeRef->RetiredConnectionIncarnations.Contains(Incarnation))
				{
					BridgeRef->RetirementBarrierPassed.Add(Incarnation);
				}
			});
		if (QueueResult != EFlecsWorkerCommandResult::Accepted)
		{
			FScopeLock Lock(&WorkerBridgeState->ResultMutex);
			WorkerBridgeState->RetirementBarrierQueued.Remove(Incarnation);
		}
	}
}

void UFlecsNetworkSessionSubsystem::RetireConnectionIncarnation(
	FConnectionIncarnation Incarnation, uint64 RetiredServerWorldEpoch)
{
	check(IsInGameThread());
	if (!Incarnation.IsValid() || RetiredServerWorldEpoch == 0 || GetWorld() == nullptr ||
		GetWorld()->GetNetMode() == NM_Client || !WorkerBridgeState.IsValid()) return;
	FScopeLock Lock(&WorkerBridgeState->ResultMutex);
	if (!WorkerBridgeState->RetiredConnectionIncarnations.Contains(Incarnation.Value))
	{
		if (WorkerBridgeState->RetiredConnectionIncarnations.Num() >= MaxPendingRetiredIncarnations)
		{
			// Keep the bounded fence set intact and close the network path. Every queued callback
			// also checks this shared fail-closed bit; recovery requires worker lifecycle replacement.
			if (!WorkerBridgeState->bRetirementFailClosed)
			{
				WorkerBridgeState->bRetirementFailClosed = true;
				bRetirementFailClosed = true;
				UE_LOG(LogTemp, Error, TEXT("Flecs networking reached %d unresolved connection retirements; network sessions are fail-closed until worker lifecycle replacement."),
					MaxPendingRetiredIncarnations);
			}
		}
		else
		{
			WorkerBridgeState->RetiredConnectionIncarnations.Add(Incarnation.Value);
			WorkerBridgeState->RetiredServerWorldEpochs.Add(Incarnation.Value, RetiredServerWorldEpoch);
		}
	}
	WorkerBridgeState->CompletedDeltas.Remove(Incarnation.Value);
	WorkerBridgeState->SpawnResults.Remove(Incarnation.Value);
	WorkerBridgeState->ClientApplyResults.Remove(Incarnation.Value);
	WorkerBridgeState->DeltaAckResults.Remove(Incarnation.Value);
	WorkerBridgeState->CommandExecutionResults.Remove(Incarnation.Value);
	WorkerBridgeState->CommandResultOverflowIncarnations.Remove(Incarnation.Value);
}

void UFlecsNetworkSessionSubsystem::QueueClientModelReset(
	FConnectionIncarnation Incarnation, uint64 ExpectedLocalEpoch)
{
	check(IsInGameThread());
	if (!Incarnation.IsValid() || ExpectedLocalEpoch == 0 || !WorkerBridgeState.IsValid()) return;
	{
		FScopeLock Lock(&WorkerBridgeState->ResultMutex);
		WorkerBridgeState->ClientApplyResults.Remove(Incarnation.Value);
	}
	if (PendingClientModelResetIncarnation == 0)
	{
		PendingClientModelResetIncarnation = Incarnation.Value;
		PendingClientModelResetLocalEpoch = ExpectedLocalEpoch;
	}
	RetryPendingClientModelReset();
}

void UFlecsNetworkSessionSubsystem::RetryPendingClientModelReset()
{
	check(IsInGameThread());
	if (PendingClientModelResetIncarnation == 0 || !WorkerBridgeState.IsValid()) return;
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (!FlecsSubsystem) return;
	const uint64 Incarnation = PendingClientModelResetIncarnation;
	const uint64 ExpectedLocalEpoch = PendingClientModelResetLocalEpoch;
	if (ExpectedLocalEpoch != FlecsSubsystem->GetActiveEpoch())
	{
		// The old worker lifecycle is stopping or has already shut down its client model.
		PendingClientModelResetIncarnation = 0;
		PendingClientModelResetLocalEpoch = 0;
		return;
	}
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(ExpectedLocalEpoch,
		[BridgeRef, Incarnation, ExpectedLocalEpoch](flecs::world&)
		{
			check(!IsInGameThread());
			if (BridgeRef->LocalAttachmentEpoch == ExpectedLocalEpoch &&
				BridgeRef->ClientModelIncarnation == Incarnation)
			{
				if (BridgeRef->Model && BridgeRef->bModelInitialized)
				{
					BridgeRef->Model->Shutdown();
				}
				BridgeRef->Model.Reset();
				BridgeRef->bModelInitialized = false;
				BridgeRef->ClientModelIncarnation = 0;
				BridgeRef->ServerWorldEpoch = 0;
				BridgeRef->SimulationTick = 0;
				BridgeRef->LastClientSnapshotRevision = 0;
			}
			FScopeLock Lock(&BridgeRef->ResultMutex);
			BridgeRef->ClientApplyResults.Remove(Incarnation);
		});
	if (QueueResult == EFlecsWorkerCommandResult::Accepted ||
		QueueResult == EFlecsWorkerCommandResult::StaleEpoch || QueueResult == EFlecsWorkerCommandResult::Stopping)
	{
		PendingClientModelResetIncarnation = 0;
		PendingClientModelResetLocalEpoch = 0;
	}
}

void UFlecsNetworkSessionSubsystem::ProcessServerCommandResults()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client || !WorkerBridgeState.IsValid())
	{
		return;
	}
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		TMap<uint32, FFlecsGenericWorkerBridgeState::FCommandExecutionResult> Results;
		bool bOverflow = false;
		{
			FScopeLock Lock(&WorkerBridgeState->ResultMutex);
			bOverflow = WorkerBridgeState->CommandResultOverflowIncarnations.Remove(Session.Incarnation.Value) > 0;
			if (TMap<uint32, FFlecsGenericWorkerBridgeState::FCommandExecutionResult>* Found =
				WorkerBridgeState->CommandExecutionResults.Find(Session.Incarnation.Value))
			{
				Results = MoveTemp(*Found);
				WorkerBridgeState->CommandExecutionResults.Remove(Session.Incarnation.Value);
			}
		}
		if (bOverflow)
		{
			RejectPeerManifest(Session.Incarnation, TEXT("The bounded worker command result mailbox overflowed."));
			continue;
		}
		if (Results.IsEmpty())
		{
			continue;
		}
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		if (Session.State != EFlecsPeerSessionState::Compatible || !IsValid(Channel) ||
			Session.LocalAttachmentEpoch != LocalAttachmentEpoch || Session.ServerWorldEpoch == 0)
		{
			continue;
		}
		for (const TPair<uint32, FFlecsGenericWorkerBridgeState::FCommandExecutionResult>& PairResult : Results)
		{
			const FFlecsGenericWorkerBridgeState::FCommandExecutionResult& Result = PairResult.Value;
			if (Result.ServerWorldEpoch != Session.ServerWorldEpoch ||
				!Channel->ReportDiscreteCommandExecutionResult(Result.ServerWorldEpoch, Result.Sequence,
					ToCommandExecutionResult(Result.Result)))
			{
				RejectPeerManifest(Session.Incarnation, TEXT("The channel rejected a worker command completion result."));
				break;
			}
			EmitPeerEvent(Session, EFlecsSessionEventKind::ServerCommandFinished, 0, 0,
				Result.Sequence, ToCommandExecutionResult(Result.Result));
		}
	}
}

void UFlecsNetworkSessionSubsystem::ProcessServerReplication()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (bRetirementFailClosed || !World || World->GetNetMode() == NM_Client || !FlecsSubsystem || !WorkerBridgeState.IsValid())
	{
		return;
	}
	if (WorkerBridgeState->bAuthoritativeModelFailed.load(std::memory_order_acquire))
	{
		UE_LOG(LogTemp, Error, TEXT("Flecs authoritative worker model failed; closing network sessions until worker replacement."));
		for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
		{
			RejectPeerManifest(Pair.Value.Incarnation, TEXT("The authoritative simulation worker is unavailable."));
		}
		bRetirementFailClosed = true;
		return;
	}
	const TSharedPtr<const FFlecsSimulationDataPublication, ESPMode::ThreadSafe> Publication =
		FlecsSubsystem->GetDataPublication();
	if (!Publication.IsValid() || Publication->LocalAttachmentEpoch != LocalAttachmentEpoch ||
		Publication->SimulationTick == 0)
	{
		return;
	}
	FFlecsSpatialSnapshot Snapshot;
	TMap<FFlecsNetworkEntityKey, FString> DefinitionStableIdsByEntity;
	Snapshot.WorldEpoch = ServerWorldEpoch;
	uint32 PublishedModelTick = 0;
	uint32 CompletedModelTick = 0;
	bool bReceivedCompleteLiveIdentitySet = false;
	bool bReceivedSpatialCompletion = false;
	for (const FFlecsSimulationDataItem& Item : Publication->Items)
	{
		if (Item.TypeId == SpatialPublicationCompleteTypeId)
		{
			if (bReceivedSpatialCompletion || Item.Payload.Num() != 4) return;
			int32 Offset = 0;
			CompletedModelTick = ReadU32(Item.Payload, Offset);
			bReceivedSpatialCompletion = true;
			continue;
		}
		if (Item.TypeId == LiveIdentityTypeId)
		{
			if (bReceivedCompleteLiveIdentitySet || !DecodeLiveIdentities(Item.Payload, Snapshot.WorldEpoch,
				PublishedModelTick, Snapshot.LiveEntityKeys))
			{
				return;
			}
			bReceivedCompleteLiveIdentitySet = true;
			continue;
		}
		if (Item.TypeId != SpatialMetadataTypeId)
		{
			continue;
		}
		FFlecsSpatialEntity Entity;
		FString DefinitionStableId;
		if (!DecodeSpatialMetadata(Item.Payload, Entity, DefinitionStableId))
		{
			return;
		}
		if (Entity.Key.WorldEpoch != Snapshot.WorldEpoch)
		{
			return;
		}
		DefinitionStableIdsByEntity.Add(Entity.Key, MoveTemp(DefinitionStableId));
		Snapshot.Entities.Add(MoveTemp(Entity));
	}
	if (!bReceivedCompleteLiveIdentitySet || !bReceivedSpatialCompletion || CompletedModelTick != PublishedModelTick ||
		Snapshot.WorldEpoch == 0 || Snapshot.WorldEpoch != ServerWorldEpoch)
	{
		return;
	}
	Snapshot.bHasCompleteLiveEntitySet = true;
	Snapshot.SimulationTick = PublishedModelTick;
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		if (Session.State == EFlecsPeerSessionState::Compatible && IsValid(Channel) &&
			Session.ServerWorldEpoch == ServerWorldEpoch)
		{
			Channel->PublishServerTickHeartbeat(ServerWorldEpoch, PublishedModelTick);
		}
	}

	FFlecsSpatialIndex SpatialIndex;
	if (!SpatialIndex.Build(Snapshot))
	{
		return;
	}
	const double NowSeconds = FPlatformTime::Seconds();
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		if (Session.State != EFlecsPeerSessionState::Compatible || !IsValid(Channel) ||
			Session.ServerWorldEpoch != Snapshot.WorldEpoch || !Session.Runtime.IsValid())
		{
			continue;
		}
		FFlecsGenericPeerSchedulingRuntime& Runtime = *Session.Runtime;
		FFlecsGenericWorkerBridgeState::FSpawnResult SpawnResult;
		bool bHasSpawnResult = false;
		{
			FScopeLock Lock(&WorkerBridgeState->ResultMutex);
			if (FFlecsGenericWorkerBridgeState::FSpawnResult* Found =
				WorkerBridgeState->SpawnResults.Find(Session.Incarnation.Value))
			{
				SpawnResult = *Found;
				WorkerBridgeState->SpawnResults.Remove(Session.Incarnation.Value);
				bHasSpawnResult = true;
			}
		}
		if (bHasSpawnResult)
		{
			if (!Runtime.bSpawnPending || SpawnResult.RequestId != Runtime.PendingSpawnRequestId ||
				SpawnResult.LocalAttachmentEpoch != LocalAttachmentEpoch || SpawnResult.ServerWorldEpoch != Session.ServerWorldEpoch)
			{
				RejectPeerManifest(Session.Incarnation, TEXT("The worker returned a stale peer spawn result."));
				continue;
			}
			const bool bWasInitial = Runtime.bPendingInitialSpawn;
			Runtime.bSpawnPending = false;
			Runtime.bPendingInitialSpawn = false;
			Runtime.PendingSpawnRequestId = 0;
			FFlecsSessionEvent Event;
			Event.Kind = EFlecsSessionEventKind::PeerSpawnFinished;
			Event.Incarnation = Session.Incarnation;
			Event.Controller = Session.Controller;
			Event.Channel = Session.Channel;
			Event.ServerWorldEpoch = Session.ServerWorldEpoch;
			Event.SpawnRequestId = SpawnResult.RequestId;
			Event.SpawnResult = SpawnResult.Result;
			Event.SpawnedIdentity = SpawnResult.Identity;
			OnSessionEvent.Broadcast(Event);
			if (bWasInitial && SpawnResult.Result != FlecsSimulation::EModelResult::Accepted)
			{
				RejectPeerManifest(Session.Incarnation, TEXT("The worker rejected the peer's initial entity spawn."));
				continue;
			}
		}
		if (!Runtime.bPeerSpawnQueued && !Runtime.bSpawnPending && PeerSpawnFactory)
		{
			APlayerController* SpawnController = Pair.Key.Get();
			if (!IsValid(SpawnController))
			{
				continue;
			}
			FFlecsPeerSpawnRequest SpawnRequest;
			FString SpawnError;
			FFlecsPeerSpawnContext SpawnContext;
			SpawnContext.Incarnation = Session.Incarnation;
			SpawnContext.ServerWorldEpoch = Session.ServerWorldEpoch;
			SpawnContext.Controller = SpawnController;
			SpawnContext.ViewPosition = SpawnController->GetFocalLocation();
			if (!PeerSpawnFactory(SpawnContext, SpawnRequest, SpawnError) ||
				SpawnRequest.DefinitionStableId.IsEmpty())
			{
				RejectPeerManifest(Session.Incarnation, SpawnError.IsEmpty() ?
					TEXT("The peer spawn factory returned an invalid request.") : SpawnError.Left(512));
				continue;
			}
			uint64 RequestId = 0;
			if (!QueuePeerEntitySpawn(Session.Incarnation, MoveTemp(SpawnRequest), RequestId, SpawnError))
			{
				RejectPeerManifest(Session.Incarnation, SpawnError.IsEmpty() ?
					TEXT("Could not queue the peer's initial entity spawn.") : SpawnError.Left(512));
				continue;
			}
			Runtime.bPendingInitialSpawn = true;
			Runtime.bPeerSpawnQueued = true;
		}
		if (Runtime.bModelAckPending)
		{
			TryQueueDeltaAcknowledgement(Session);
			FFlecsGenericWorkerBridgeState::FDeltaAckResult AckResult;
			bool bHasAckResult = false;
			{
				FScopeLock Lock(&WorkerBridgeState->ResultMutex);
				if (FFlecsGenericWorkerBridgeState::FDeltaAckResult* Found =
					WorkerBridgeState->DeltaAckResults.Find(Session.Incarnation.Value))
				{
					AckResult = *Found;
					WorkerBridgeState->DeltaAckResults.Remove(Session.Incarnation.Value);
					bHasAckResult = true;
				}
			}
			if (bHasAckResult)
			{
				const bool bAckMatches = AckResult.LocalAttachmentEpoch == LocalAttachmentEpoch &&
					AckResult.ServerWorldEpoch == Session.ServerWorldEpoch &&
					AckResult.ModelPublicationRevision == Runtime.PendingModelRevision &&
					AckResult.SelectionId == Runtime.PendingSelection.SelectionId;
				if (!bAckMatches || AckResult.Result != FlecsSimulation::EModelResult::Accepted)
				{
					RejectPeerManifest(Session.Incarnation, TEXT("The worker could not commit the acknowledged peer delta."));
					continue;
				}
				Runtime.SelectedEntityKeys = MoveTemp(Runtime.PendingEntityKeys);
				Runtime.Scheduler.CompleteSelection(Runtime.PendingSelection.SelectionId, true, NowSeconds);
				Runtime.PendingSelection = FFlecsRelevanceSelection();
				Runtime.PendingWireRecords.Reset();
				Runtime.PendingSimulationTick = 0;
				Runtime.PendingModelRevision = 0;
				Runtime.TransferRetryCount = 0;
				Runtime.bModelAckPending = false;
				Runtime.bModelAckQueued = false;
				Runtime.bTransferSucceeded = false;
			}
			continue;
		}

		if (Runtime.bTransferPending || Channel->IsUpdateBatchTransferActive())
		{
			continue;
		}

		if (Runtime.bSnapshotRequestPending)
		{
			FFlecsGenericWorkerBridgeState::FCompletedDelta Completed;
			bool bHasCompleted = false;
			{
				FScopeLock Lock(&WorkerBridgeState->ResultMutex);
				if (FFlecsGenericWorkerBridgeState::FCompletedDelta* Found =
					WorkerBridgeState->CompletedDeltas.Find(Session.Incarnation.Value))
				{
					Completed = MoveTemp(*Found);
					WorkerBridgeState->CompletedDeltas.Remove(Session.Incarnation.Value);
					bHasCompleted = true;
				}
			}
			if (!bHasCompleted)
			{
				continue;
			}
			const bool bCurrentSelection = Completed.LocalAttachmentEpoch == LocalAttachmentEpoch &&
				Completed.ServerWorldEpoch == Session.ServerWorldEpoch &&
				Completed.ConnectionIncarnation == Session.Incarnation.Value &&
				Completed.SelectionId == Runtime.PendingSelection.SelectionId;
			if (bCurrentSelection && Completed.Result == FlecsSimulation::EModelResult::StaleIdentity)
			{
				// The worker can retire or replace an entity after the game-thread spatial
				// publication was selected. No delta was staged in this case; select
				// again from a newer publication instead of disconnecting the peer.
				Runtime.Scheduler.CompleteSelection(Runtime.PendingSelection.SelectionId, false, NowSeconds);
				Runtime.PendingSelection = FFlecsRelevanceSelection();
				Runtime.PendingEntityKeys.Empty();
				Runtime.bSnapshotRequestPending = false;
				continue;
			}
			bool bValidDelta = bCurrentSelection &&
				Completed.Result == FlecsSimulation::EModelResult::Accepted &&
				!Completed.Batch.bCompleteStateSnapshot;
			TArray<FFlecsReplicationRecord> WireRecords;
			if (bValidDelta)
			{
				WireRecords.Reserve(Completed.Batch.Records.Num());
				for (const FlecsSimulation::FStateRecord& StateRecord : Completed.Batch.Records)
				{
					FFlecsReplicationRecord& Record = WireRecords.AddDefaulted_GetRef();
					Record.WorldEpoch = StateRecord.Identity.ServerWorldEpoch;
					Record.NetworkEntityId = StateRecord.Identity.NetworkEntityId;
					Record.EntityGeneration = StateRecord.Identity.Generation;
					Record.DefinitionWireId = StateRecord.DefinitionWireId;
					Record.ComponentWireId = StateRecord.ComponentWireId;
					Record.SourceSimulationTick = StateRecord.SourceSimulationTick;
					Record.Payload = StateRecord.Payload;
					switch (StateRecord.Operation)
					{
					case FlecsSimulation::EStateOperation::ComponentUpsert: Record.Operation = 0; break;
					case FlecsSimulation::EStateOperation::ComponentRemove: Record.Operation = 1; break;
					case FlecsSimulation::EStateOperation::EntityDespawn: Record.Operation = 2; break;
					case FlecsSimulation::EStateOperation::EntityCreate: Record.Operation = 3; break;
					case FlecsSimulation::EStateOperation::EntityLeaveRelevance: Record.Operation = 4; break;
					default: bValidDelta = false; break;
					}
					if (!bValidDelta) break;
				}
			}
			uint64 EncodedBytes = 0;
			for (const FFlecsReplicationRecord& Record : WireRecords)
			{
				EncodedBytes += 45ull + static_cast<uint64>(Record.Payload.Num()); // Channel framing budget.
			}
			if (!bValidDelta || WireRecords.Num() > MaxChannelUpdateRecords || EncodedBytes > MaxChannelUpdatePayloadBytes)
			{
				RejectPeerManifest(Session.Incarnation, TEXT("The worker produced an invalid or over-limit peer update batch."));
				continue;
			}
			Runtime.PendingModelRevision = Completed.Batch.PublicationRevision;
			Runtime.PendingWireRecords = MoveTemp(WireRecords);
			Runtime.PendingSimulationTick = Completed.Batch.SimulationTick;
			Runtime.bSnapshotRequestPending = false;
			if (Runtime.PendingWireRecords.IsEmpty())
			{
				if (Runtime.PendingModelRevision == 0)
				{
					Runtime.SelectedEntityKeys = MoveTemp(Runtime.PendingEntityKeys);
					Runtime.Scheduler.CompleteSelection(Runtime.PendingSelection.SelectionId, true, NowSeconds);
					Runtime.PendingSelection = FFlecsRelevanceSelection();
					Runtime.PendingSimulationTick = 0;
				}
				else
				{
					Runtime.bModelAckPending = true;
					Runtime.bTransferSucceeded = true;
					TryQueueDeltaAcknowledgement(Session);
				}
				continue;
			}
			if (Runtime.PendingModelRevision == 0)
			{
				RejectPeerManifest(Session.Incarnation, TEXT("The worker produced wire records without a model publication revision."));
				continue;
			}
			if (!Channel->PublishUpdateBatch(Session.ServerWorldEpoch, Runtime.PendingWireRecords,
				Runtime.PendingSimulationTick))
			{
				const FString ChannelReason = Channel->GetLastUpdateBatchRejectionReason();
				FString Reason = TEXT("Iris refused the bounded peer update batch without a diagnostic.");
				if (!ChannelReason.IsEmpty())
				{
					Reason = FString::Printf(TEXT("Iris refused the bounded peer update batch: %s"), *ChannelReason);
				}
				RejectPeerManifest(Session.Incarnation, Reason.Left(512));
				continue;
			}
			Runtime.bTransferPending = true;
			Runtime.TransferRetryCount = 0;
			EmitPeerEvent(Session, EFlecsSessionEventKind::ServerTransferStarted,
				Runtime.PendingModelRevision, Runtime.PendingSimulationTick);
			continue;
		}

		if (Runtime.bSnapshotRequestPending)
		{
			continue;
		}
		APlayerController* Controller = Pair.Key.Get();
		FlecsModRegistry::FSessionMapping Mapping;
		if (!IsValid(Controller) || !GetPeerSessionMapping(Session.Incarnation, Mapping))
		{
			continue;
		}
		FFlecsConnectionInterest Interest;
		Interest.ConnectionId = Session.Incarnation.Value;
		const FVector FocalLocation = Controller->GetFocalLocation();
		Interest.ViewPosition = FVector3f(static_cast<float>(FocalLocation.X),
			static_cast<float>(FocalLocation.Y), static_cast<float>(FocalLocation.Z));
		Interest.Tiers = FFlecsPerConnectionScheduler::MakeDefaultTiers();
		FFlecsRelevanceBudget Budget;
		Budget.MaxRecords = 128;
		Budget.MaxBytes = 64 * 1024;
		if (PeerInterestProvider)
		{
			FFlecsPeerSpawnContext Context;
			Context.Incarnation = Session.Incarnation;
			Context.ServerWorldEpoch = Session.ServerWorldEpoch;
			Context.Controller = Controller;
			Context.ViewPosition = FocalLocation;
			FString InterestError;
			if (!PeerInterestProvider(Context, Interest, Budget, InterestError) || Budget.MaxRecords == 0 ||
				Budget.MaxRecords > static_cast<uint32>(MaxChannelUpdateRecords) || Budget.MaxBytes == 0 || Budget.MaxBytes > MaxChannelUpdatePayloadBytes)
			{
				RejectPeerManifest(Session.Incarnation, InterestError.IsEmpty() ?
					TEXT("The peer interest provider returned an invalid or over-limit budget.") : InterestError.Left(512));
				continue;
			}
			// The game can choose relevance but cannot impersonate another connection.
			Interest.ConnectionId = Session.Incarnation.Value;
		}
		FFlecsRelevanceSelection Selection;
		if (!Runtime.Scheduler.Select(SpatialIndex, Interest, NowSeconds, Budget, Selection))
		{
			continue;
		}
		if (Selection.Metrics.OversizedEntityCount > 0)
		{
			const FFlecsNetworkEntityKey& Oversized = Selection.Metrics.FirstOversizedEntity;
			RejectPeerManifest(Session.Incarnation,
				FString::Printf(TEXT("Entity %u generation %u has an indivisible update estimate above the per-peer byte budget."),
					Oversized.EntityId, static_cast<uint32>(Oversized.Generation)));
			continue;
		}
		if (Selection.Records.IsEmpty())
		{
			continue;
		}

		TSet<uint64> CandidateKeys = Runtime.SelectedEntityKeys;
		for (const FFlecsScheduledEntity& Scheduled : Selection.Records)
		{
			const uint64 Key = FlecsSimulation::FPeerReplicationContext::MakeEntityViewKey(
				Scheduled.Key.EntityId, Scheduled.Key.Generation);
			if (Scheduled.Transition == EFlecsRelevanceTransition::Exit)
			{
				CandidateKeys.Remove(Key);
			}
			else
			{
				CandidateKeys.Add(Key);
			}
		}
		FlecsSimulation::FPeerReplicationContext PeerContext;
		PeerContext.ConnectionToken = Session.Incarnation.Value;
		TArray<FlecsSimulation::FSelectedEntityComponents> DueSelection;
		bool bDueMetadataValid = true;
		for (const FFlecsScheduledEntity& Scheduled : Selection.Records)
		{
			const uint64 Key = FlecsSimulation::FPeerReplicationContext::MakeEntityViewKey(
				Scheduled.Key.EntityId, Scheduled.Key.Generation);
			if (Scheduled.Transition == EFlecsRelevanceTransition::Exit)
			{
				continue;
			}
			const FFlecsSpatialEntity* Metadata = Snapshot.Entities.FindByPredicate(
				[&Scheduled](const FFlecsSpatialEntity& Entity)
				{
					return Entity.Key == Scheduled.Key;
				});
			const FString* DefinitionStableId = Metadata ? DefinitionStableIdsByEntity.Find(Scheduled.Key) : nullptr;
			if (!Metadata || !DefinitionStableId)
			{
				bDueMetadataValid = false;
				break;
			}
			FlecsSimulation::FSelectedEntityComponents& Due = DueSelection.AddDefaulted_GetRef();
			Due.Identity.ServerWorldEpoch = Metadata->Key.WorldEpoch;
			Due.Identity.NetworkEntityId = Metadata->Key.EntityId;
			Due.Identity.Generation = Metadata->Key.Generation;
			Due.Identity.DefinitionStableId = *DefinitionStableId;
			Due.bAllVisibleComponentsDue = true;
		}
		if (!bDueMetadataValid)
		{
			Runtime.Scheduler.CompleteSelection(Selection.SelectionId, false, NowSeconds);
			continue;
		}
		for (uint64 Key : CandidateKeys)
		{
			const uint32 EntityId = static_cast<uint32>(Key >> 16);
			const uint16 Generation = static_cast<uint16>(Key);
			PeerContext.RelevantEntityKeys.Add(FlecsSimulation::FPeerReplicationContext::MakeEntityViewKey(EntityId, Generation));
		}
		const uint64 LocalEpoch = LocalAttachmentEpoch;
		const uint64 ServerEpoch = Session.ServerWorldEpoch;
		const uint64 ConnectionIncarnation = Session.Incarnation.Value;
		const uint64 SelectionId = Selection.SelectionId;
		const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
		TSet<uint64> PendingKeys = CandidateKeys;
		const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
			[BridgeRef, PeerContext = MoveTemp(PeerContext), DueSelection = MoveTemp(DueSelection), Mapping,
				LocalEpoch, ServerEpoch, ConnectionIncarnation, SelectionId](flecs::world&) mutable
			{
				FFlecsGenericWorkerBridgeState::FCompletedDelta Completed;
				Completed.LocalAttachmentEpoch = LocalEpoch;
				Completed.ServerWorldEpoch = ServerEpoch;
				Completed.ConnectionIncarnation = ConnectionIncarnation;
				Completed.SelectionId = SelectionId;
				bool bRetired = false;
				{
					FScopeLock Lock(&BridgeRef->ResultMutex);
				bRetired = BridgeRef->bRetirementFailClosed || BridgeRef->RetiredConnectionIncarnations.Contains(ConnectionIncarnation);
				}
				if (!bRetired && BridgeRef->LocalAttachmentEpoch == LocalEpoch && BridgeRef->ServerWorldEpoch == ServerEpoch &&
					BridgeRef->Model && BridgeRef->bModelInitialized && BridgeRef->SimulationTick != 0)
				{
					Completed.Result = BridgeRef->Model->BuildAuthoritativeDelta(
						BridgeRef->SimulationTick, PeerContext, Mapping,
						TConstArrayView<FlecsSimulation::FSelectedEntityComponents>(DueSelection), Completed.Batch);
					Completed.ModelPublicationRevision = Completed.Batch.PublicationRevision;
				}
				FScopeLock Lock(&BridgeRef->ResultMutex);
				if (!BridgeRef->bRetirementFailClosed && !BridgeRef->RetiredConnectionIncarnations.Contains(ConnectionIncarnation))
				{
					BridgeRef->CompletedDeltas.Add(ConnectionIncarnation, MoveTemp(Completed));
				}
			});
		if (QueueResult != EFlecsWorkerCommandResult::Accepted)
		{
			Runtime.Scheduler.CompleteSelection(Selection.SelectionId, false, NowSeconds);
			continue;
		}
		Runtime.PendingSelection = MoveTemp(Selection);
		Runtime.PendingEntityKeys = MoveTemp(PendingKeys);
		Runtime.bSnapshotRequestPending = true;
	}
}

TStatId UFlecsNetworkSessionSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFlecsNetworkSessionSubsystem, STATGROUP_Tickables);
}

bool UFlecsNetworkSessionSubsystem::IsTickable() const
{
	return !bIsClosing && GetWorld() != nullptr;
}

void UFlecsNetworkSessionSubsystem::GetPeerSessions(
	TArray<FFlecsPeerSessionView>& OutSessions) const
{
	check(IsInGameThread());
	OutSessions.Reset(PeerSessions.Num());
	for (const TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		const FPeerSession& Session = Pair.Value;
		FFlecsPeerSessionView& View = OutSessions.AddDefaulted_GetRef();
		View.Incarnation = Session.Incarnation;
		View.State = Session.State;
		View.LocalAttachmentEpoch = Session.LocalAttachmentEpoch;
		View.ServerWorldEpoch = Session.ServerWorldEpoch;
		View.Controller = Session.Controller;
		View.Channel = Session.Channel;
		View.RejectionReason = Session.RejectionReason;
	}
}

FlecsModRegistry::FRegistry* UFlecsNetworkSessionSubsystem::GetModRegistryForSetup()
{
	check(IsInGameThread());
	if (bModRegistryFrozen || bIsClosing)
	{
		return nullptr;
	}
	return &ModRegistry;
}

bool UFlecsNetworkSessionSubsystem::SetPeerSpawnFactory(
	FFlecsPeerSpawnFactory Factory, FString& OutError)
{
	check(IsInGameThread());
	if (bModRegistryFrozen || bIsClosing)
	{
		OutError = TEXT("Peer spawn configuration is frozen or the session is closing.");
		return false;
	}
	PeerSpawnFactory = MoveTemp(Factory);
	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::SetPeerInterestProvider(
	FFlecsPeerInterestProvider Provider, FString& OutError)
{
	check(IsInGameThread());
	if (bModRegistryFrozen || bIsClosing)
	{
		OutError = TEXT("Peer interest configuration is frozen or the session is closing.");
		return false;
	}
	PeerInterestProvider = MoveTemp(Provider);
	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::QueuePeerEntitySpawn(FConnectionIncarnation Incarnation,
	FFlecsPeerSpawnRequest Request, uint64& OutRequestId, FString& OutError)
{
	check(IsInGameThread());
	OutRequestId = 0;
	UWorld* World = GetWorld();
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	FPeerSession* Session = FindSession(Incarnation);
	if (bIsClosing || bRetirementFailClosed || !World || World->GetNetMode() == NM_Client ||
		!FlecsSubsystem || !WorkerBridgeState.IsValid() || !Session || !Session->Runtime.IsValid() ||
		Session->State != EFlecsPeerSessionState::Compatible || Session->LocalAttachmentEpoch != LocalAttachmentEpoch ||
		Session->ServerWorldEpoch == 0 || Session->ServerWorldEpoch != ServerWorldEpoch ||
		WorkerBridgeState->bAuthoritativeModelFailed.load(std::memory_order_acquire))
	{
		OutError = TEXT("The peer or authoritative worker is unavailable.");
		return false;
	}
	FFlecsGenericPeerSchedulingRuntime& Runtime = *Session->Runtime;
	if (Runtime.bSpawnPending || NextSpawnRequestId == MAX_uint64)
	{
		OutError = TEXT("The peer already has a pending spawn or request IDs are exhausted.");
		return false;
	}
	if (!ModRegistry.FindEntityDefinition(Request.DefinitionStableId) ||
		Request.InitialComponents.Num() > FlecsSimulation::MaxComponentsPerEntity)
	{
		OutError = TEXT("The spawn definition or component count is invalid.");
		return false;
	}
	uint64 TotalBytes = 0;
	for (const FlecsSimulation::FInitialComponent& Component : Request.InitialComponents)
	{
		TotalBytes += static_cast<uint32>(Component.Payload.Num());
		if (Component.Payload.Num() > static_cast<int32>(MaxTransportSchemaPayloadBytes) ||
			TotalBytes > FlecsSimulation::MaxBatchBytes)
		{
			OutError = TEXT("The spawn request exceeds the bounded component payload limits.");
			return false;
		}
	}
	const uint64 RequestId = NextSpawnRequestId + 1;
	const uint64 LocalEpoch = Session->LocalAttachmentEpoch;
	const uint64 ServerEpoch = Session->ServerWorldEpoch;
	const uint64 Connection = Incarnation.Value;
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
		[BridgeRef, Request = MoveTemp(Request), LocalEpoch, ServerEpoch, Connection, RequestId](flecs::world&) mutable
		{
			FFlecsGenericWorkerBridgeState::FSpawnResult Result;
			Result.LocalAttachmentEpoch = LocalEpoch;
			Result.ServerWorldEpoch = ServerEpoch;
			Result.RequestId = RequestId;
			bool bRetired = false;
			{
				FScopeLock Lock(&BridgeRef->ResultMutex);
				bRetired = BridgeRef->bRetirementFailClosed || BridgeRef->RetiredConnectionIncarnations.Contains(Connection);
			}
			if (!bRetired && BridgeRef->LocalAttachmentEpoch == LocalEpoch && BridgeRef->ServerWorldEpoch == ServerEpoch &&
				BridgeRef->Model && BridgeRef->bModelInitialized)
			{
				TArray<FlecsSimulation::FNetworkEntityIdentity>* Owned = BridgeRef->OwnedEntitiesByConnection.Find(Connection);
				if (Owned && Owned->Num() >= MaxPeerOwnedEntities)
				{
					Result.Result = FlecsSimulation::EModelResult::LimitReached;
				}
				else
				{
					Result.Result = BridgeRef->Model->SpawnAuthoritative(Request.DefinitionStableId, Connection,
						TConstArrayView<FlecsSimulation::FInitialComponent>(Request.InitialComponents), Result.Identity);
					if (Result.Result == FlecsSimulation::EModelResult::Accepted)
						BridgeRef->OwnedEntitiesByConnection.FindOrAdd(Connection).Add(Result.Identity);
				}
			}
			FScopeLock Lock(&BridgeRef->ResultMutex);
			if (!BridgeRef->bRetirementFailClosed && !BridgeRef->RetiredConnectionIncarnations.Contains(Connection))
				BridgeRef->SpawnResults.Add(Connection, MoveTemp(Result));
		});
	if (QueueResult != EFlecsWorkerCommandResult::Accepted)
	{
		OutError = FString::Printf(TEXT("The worker could not queue the peer spawn (result %u)."), static_cast<uint32>(QueueResult));
		return false;
	}
	NextSpawnRequestId = RequestId;
	Runtime.bSpawnPending = true;
	Runtime.bPendingInitialSpawn = false;
	Runtime.PendingSpawnRequestId = RequestId;
	OutRequestId = RequestId;
	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::RegisterTrustedPermissionRule(
	const FString& PermissionStableId, FFlecsTrustedPermissionRule Rule, FString& OutError)
{
	check(IsInGameThread());
	if (bModRegistryFrozen || bIsClosing || PermissionStableId.IsEmpty() || !Rule ||
		TrustedPermissionRules.Contains(PermissionStableId))
	{
		OutError = TEXT("Trusted permission registration is invalid, duplicated, or too late.");
		return false;
	}
	TrustedPermissionRules.Add(PermissionStableId, MoveTemp(Rule));
	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::RegisterTrustedSpatialDefinition(
	const FString& DefinitionStableId, EFlecsTrustedSpatialPolicy Policy,
	FFlecsTrustedPositionAccessor Accessor, FString& OutError)
{
	check(IsInGameThread());
	const bool bPositionRequired = Policy == EFlecsTrustedSpatialPolicy::PositionRequired;
	if (bModRegistryFrozen || bIsClosing || DefinitionStableId.IsEmpty() ||
		!ModRegistry.FindEntityDefinition(DefinitionStableId) || TrustedSpatialDefinitions.Contains(DefinitionStableId) ||
		(bPositionRequired && !Accessor) || (!bPositionRequired && Accessor))
	{
		OutError = TEXT("Trusted spatial registration must name a registered entity definition and match its position policy before freeze.");
		return false;
	}
	FFlecsTrustedSpatialDefinition Registration;
	Registration.Policy = Policy;
	Registration.PositionAccessor = MoveTemp(Accessor);
	TrustedSpatialDefinitions.Add(DefinitionStableId, MoveTemp(Registration));
	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::FreezeModRegistry(uint32 ProtocolVersion, FString& OutError)
{
	check(IsInGameThread());
	if (bModRegistryFrozen)
	{
		OutError = TEXT("The network session mod registry is already frozen.");
		return false;
	}
	if (UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
	{
		const UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
		if (!FlecsSubsystem || FlecsSubsystem->GetActiveEpoch() == 0)
		{
			OutError = TEXT("Client mod registry freeze is deferred until the Flecs worker is attached.");
			return false;
		}
	}
	if (!ModRegistry.Freeze(ProtocolVersion, OutError))
	{
		return false;
	}
	FlecsModRegistry::FManifestDescriptor Descriptor;
	if (!ModRegistry.ExportManifestDescriptor(Descriptor, OutError) || !Descriptor.Serialize(LocalManifestDescriptorBytes, OutError))
	{
		return false;
	}
	if (!InstallWorkerIntegration(OutError))
	{
		return false;
	}
	bModRegistryFrozen = true;
	return true;
}

bool UFlecsNetworkSessionSubsystem::InstallWorkerIntegration(FString& OutError)
{
	check(IsInGameThread());
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (!FlecsSubsystem || !WorkerBridgeState.IsValid())
	{
		OutError = TEXT("The Flecs worker or example mod state is unavailable.");
		return false;
	}

	FrozenWorkerRegistry = MakeShared<FlecsModRegistry::FRegistry, ESPMode::ThreadSafe>(ModRegistry);
	const TSharedRef<FlecsModRegistry::FRegistry, ESPMode::ThreadSafe> RegistryRef = FrozenWorkerRegistry.ToSharedRef();
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const TMap<FString, FFlecsTrustedPermissionRule> PermissionRules = TrustedPermissionRules;
	const TMap<FString, FFlecsTrustedSpatialDefinition> SpatialDefinitions = TrustedSpatialDefinitions;
	const bool bAuthoritative = GetWorld() && GetWorld()->GetNetMode() != NM_Client;
	const uint64 CapturedServerWorldEpoch = bAuthoritative ? ServerWorldEpoch : 0;
	const uint64 ExpectedLocalEpoch = FlecsSubsystem->GetActiveEpoch();

	FFlecsWorkerStartupCallback Startup = [BridgeRef, RegistryRef, PermissionRules, SpatialDefinitions,
		bAuthoritative, CapturedServerWorldEpoch]
		(flecs::world& World, uint64 LocalEpoch)
	{
		check(!IsInGameThread());
		if (BridgeRef->bLifecycleRetired.load(std::memory_order_acquire))
		{
			return;
		}
		if (BridgeRef->Model && BridgeRef->bModelInitialized)
		{
			BridgeRef->Model->Shutdown();
		}
		BridgeRef->Model.Reset();
		BridgeRef->bModelInitialized = false;
		BridgeRef->LocalAttachmentEpoch = LocalEpoch;
		BridgeRef->ServerWorldEpoch = CapturedServerWorldEpoch;
		BridgeRef->SimulationTick = 0;
		BridgeRef->LastClientSnapshotRevision = 0;
		BridgeRef->ClientModelIncarnation = 0;
		BridgeRef->bSpatialDiagnosticsReported = false;
		BridgeRef->bAuthoritativeModelFailed.store(false, std::memory_order_release);
		BridgeRef->Model = MakeUnique<FlecsSimulation::FWorkerEntityModel>(*RegistryRef, bAuthoritative);
		if (!bAuthoritative || CapturedServerWorldEpoch == 0)
		{
			return;
		}

		const FlecsSimulation::FWorkerEntityModel::FPermissionCheck PermissionCheck =
			[PermissionRules](uint64 ConnectionToken, const FString& Permission, uint64 CurrentOwnerToken,
				uint32 NetworkEntityId, uint16 Generation)
			{
				const FFlecsTrustedPermissionRule* Rule = PermissionRules.Find(Permission);
				return Rule && (*Rule)(ConnectionToken, CurrentOwnerToken, NetworkEntityId, Generation);
			};
		if (BridgeRef->Model->Initialize(World, CapturedServerWorldEpoch, PermissionCheck) !=
			FlecsSimulation::EModelResult::Accepted)
		{
			BridgeRef->Model.Reset();
			BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
			return;
		}
		for (const TPair<FString, FFlecsTrustedSpatialDefinition>& Pair : SpatialDefinitions)
		{
			FlecsSimulation::ESpatialDefinitionPolicy Policy = FlecsSimulation::ESpatialDefinitionPolicy::PositionRequired;
			switch (Pair.Value.Policy)
			{
			case EFlecsTrustedSpatialPolicy::PositionRequired:
				Policy = FlecsSimulation::ESpatialDefinitionPolicy::PositionRequired;
				break;
			case EFlecsTrustedSpatialPolicy::AlwaysRelevantWithoutPosition:
				Policy = FlecsSimulation::ESpatialDefinitionPolicy::AlwaysRelevantWithoutPosition;
				break;
			case EFlecsTrustedSpatialPolicy::SkipWithoutPosition:
				Policy = FlecsSimulation::ESpatialDefinitionPolicy::SkipWithoutPosition;
				break;
			default:
				BridgeRef->Model->Shutdown();
				BridgeRef->Model.Reset();
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			if (BridgeRef->Model->RegisterSpatialDefinition(Pair.Key, Policy, Pair.Value.PositionAccessor) !=
				FlecsSimulation::EModelResult::Accepted)
			{
				BridgeRef->Model->Shutdown();
				BridgeRef->Model.Reset();
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
		}
		BridgeRef->bModelInitialized = true;

	};

	FFlecsWorkerShutdownCallback Shutdown = [BridgeRef](flecs::world&, uint64 LocalEpoch)
	{
		check(!IsInGameThread());
		if (BridgeRef->LocalAttachmentEpoch != LocalEpoch)
		{
			return;
		}
		if (BridgeRef->Model && BridgeRef->bModelInitialized)
		{
			BridgeRef->Model->Shutdown();
		}
		BridgeRef->Model.Reset();
		BridgeRef->OwnedEntitiesByConnection.Empty();
		BridgeRef->bModelInitialized = false;
		BridgeRef->LocalAttachmentEpoch = 0;
		BridgeRef->ServerWorldEpoch = 0;
		BridgeRef->SimulationTick = 0;
		BridgeRef->LastClientSnapshotRevision = 0;
		BridgeRef->ClientModelIncarnation = 0;
		BridgeRef->bSpatialDiagnosticsReported = false;
		{
			FScopeLock Lock(&BridgeRef->ResultMutex);
			BridgeRef->CompletedDeltas.Empty();
			BridgeRef->ClientApplyResults.Empty();
			BridgeRef->SpawnResults.Empty();
			BridgeRef->DeltaAckResults.Empty();
			BridgeRef->CommandExecutionResults.Empty();
			BridgeRef->CommandResultOverflowIncarnations.Empty();
			BridgeRef->RetiredConnectionIncarnations.Empty();
			BridgeRef->RetiredServerWorldEpochs.Empty();
			BridgeRef->RetirementBarrierQueued.Empty();
			BridgeRef->RetirementBarrierPassed.Empty();
			BridgeRef->bRetirementFailClosed = false;
		}
	};

	const EFlecsWorkerCommandResult LifecycleResult = FlecsSubsystem->RegisterPersistentWorkerLifecycle(
		ExpectedLocalEpoch, FName(TEXT("FLECSNetwork.SessionModel")), MoveTemp(Startup), MoveTemp(Shutdown));
	if (LifecycleResult != EFlecsWorkerCommandResult::Accepted)
	{
		OutError = FString::Printf(TEXT("Could not install the worker model lifecycle (result %u)."),
			static_cast<uint32>(LifecycleResult));
		return false;
	}

	if (bAuthoritative)
	{
		FFlecsWorkerDataPublisher Publisher = [BridgeRef](flecs::world&, FFlecsSimulationDataBuilder& Builder)
		{
			if (BridgeRef->bLifecycleRetired.load(std::memory_order_acquire) ||
				BridgeRef->bAuthoritativeModelFailed.load(std::memory_order_acquire) ||
				!BridgeRef->Model || !BridgeRef->bModelInitialized)
			{
				return;
			}
			if (BridgeRef->SimulationTick >= MAX_uint32)
			{
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			++BridgeRef->SimulationTick;
			if (BridgeRef->Model->RefreshAuthoritativeComponentChanges(BridgeRef->SimulationTick) !=
				FlecsSimulation::EModelResult::Accepted)
			{
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
		TArray<TPair<uint64, uint64>> RetiredConnections;
		{
			FScopeLock Lock(&BridgeRef->ResultMutex);
			for (uint64 RetiredIncarnation : BridgeRef->RetirementBarrierPassed)
			{
				if (const uint64* RetiredEpoch = BridgeRef->RetiredServerWorldEpochs.Find(RetiredIncarnation))
				{
					RetiredConnections.Emplace(RetiredIncarnation, *RetiredEpoch);
				}
			}
		}
		for (const TPair<uint64, uint64>& RetiredConnection : RetiredConnections)
		{
			const uint64 RetiredIncarnation = RetiredConnection.Key;
			const uint64 RetiredEpoch = RetiredConnection.Value;
			if (RetiredEpoch != BridgeRef->ServerWorldEpoch ||
				BridgeRef->Model->DiscardAuthoritativeDelta(RetiredIncarnation, RetiredEpoch,
				0, true) != FlecsSimulation::EModelResult::Accepted)
			{
				continue;
			}
			TArray<FlecsSimulation::FNetworkEntityIdentity>* Owned =
				BridgeRef->OwnedEntitiesByConnection.Find(RetiredIncarnation);
			bool bAllRetired = true;
			if (Owned)
			{
				for (int32 Index = Owned->Num() - 1; Index >= 0; --Index)
				{
					const FlecsSimulation::EModelResult Result = BridgeRef->Model->DespawnAuthoritative((*Owned)[Index], BridgeRef->SimulationTick);
					if (Result == FlecsSimulation::EModelResult::Accepted || Result == FlecsSimulation::EModelResult::StaleIdentity)
						Owned->RemoveAtSwap(Index, 1, EAllowShrinking::No);
					else
						bAllRetired = false;
				}
			}
			if (!bAllRetired)
			{
				continue;
			}
			BridgeRef->OwnedEntitiesByConnection.Remove(RetiredIncarnation);
			FScopeLock Lock(&BridgeRef->ResultMutex);
			BridgeRef->RetiredConnectionIncarnations.Remove(RetiredIncarnation);
			BridgeRef->RetiredServerWorldEpochs.Remove(RetiredIncarnation);
			BridgeRef->RetirementBarrierQueued.Remove(RetiredIncarnation);
			BridgeRef->RetirementBarrierPassed.Remove(RetiredIncarnation);
		}
			TArray<FlecsSimulation::FSpatialEntityMetadata> Metadata;
			TArray<FlecsSimulation::FSpatialMetadataDiagnostic> Diagnostics;
			const FlecsSimulation::EModelResult SpatialResult = BridgeRef->Model->BuildSpatialMetadata(
				BridgeRef->SimulationTick, Metadata, Diagnostics);
			if (SpatialResult != FlecsSimulation::EModelResult::Accepted &&
				SpatialResult != FlecsSimulation::EModelResult::Partial)
			{
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			TArray<FlecsSimulation::FNetworkEntityIdentity> LiveIdentities;
			if (BridgeRef->Model->BuildLiveEntityIdentities(LiveIdentities) != FlecsSimulation::EModelResult::Accepted)
			{
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			TArray<uint8> LiveIdentityPayload;
			if (!EncodeLiveIdentities(BridgeRef->ServerWorldEpoch, BridgeRef->SimulationTick, LiveIdentities, LiveIdentityPayload))
			{
				UE_LOG(LogTemp, Error, TEXT("Flecs worker could not encode the bounded live identity publication."));
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			if (Builder.TryAdd(LiveIdentityTypeId, TArrayView<const uint8>(LiveIdentityPayload)) !=
				EFlecsDataPublicationAddResult::Accepted)
			{
				UE_LOG(LogTemp, Error, TEXT("Flecs worker publication has no capacity for the live identity set."));
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
				return;
			}
			if (SpatialResult == FlecsSimulation::EModelResult::Partial && !BridgeRef->bSpatialDiagnosticsReported)
			{
				BridgeRef->bSpatialDiagnosticsReported = true;
			const FString Detail = Diagnostics.IsEmpty() ? TEXT("no diagnostic detail") :
				FString::Printf(TEXT("definition %s, entity %u, code %u"),
					*Diagnostics[0].Identity.DefinitionStableId,
					Diagnostics[0].Identity.NetworkEntityId,
					static_cast<uint32>(Diagnostics[0].Code));
				UE_LOG(LogTemp, Warning, TEXT("Flecs spatial publication reported bounded metadata diagnostics: %s"), *Detail);
			}
			for (const FlecsSimulation::FSpatialEntityMetadata& Entity : Metadata)
			{
				TArray<uint8> Payload;
				if (!EncodeSpatialMetadata(Entity, Payload))
				{
					if (!BridgeRef->bSpatialDiagnosticsReported)
					{
						BridgeRef->bSpatialDiagnosticsReported = true;
						UE_LOG(LogTemp, Warning, TEXT("Flecs spatial metadata for entity %u could not be encoded; its live identity is retained."),
							Entity.Identity.NetworkEntityId);
					}
					continue;
				}
				if (Builder.TryAdd(SpatialMetadataTypeId, TArrayView<const uint8>(Payload)) !=
					EFlecsDataPublicationAddResult::Accepted)
				{
					UE_LOG(LogTemp, Error, TEXT("Flecs worker publication has no capacity for spatial metadata."));
					BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
					return;
				}
			}
			TArray<uint8> CompletionPayload;
			AppendU32(CompletionPayload, BridgeRef->SimulationTick);
			if (Builder.TryAdd(SpatialPublicationCompleteTypeId, TArrayView<const uint8>(CompletionPayload)) !=
				EFlecsDataPublicationAddResult::Accepted)
			{
				UE_LOG(LogTemp, Error, TEXT("Flecs worker publication has no capacity for its completion marker."));
				BridgeRef->bAuthoritativeModelFailed.store(true, std::memory_order_release);
			}
		};
		const EFlecsWorkerCommandResult PublisherResult = FlecsSubsystem->RegisterPersistentDataPublisher(
			ExpectedLocalEpoch, FName(TEXT("FLECSNetwork.Spatial")), MoveTemp(Publisher));
		if (PublisherResult != EFlecsWorkerCommandResult::Accepted)
		{
			OutError = FString::Printf(TEXT("Could not install the worker spatial publisher (result %u)."),
				static_cast<uint32>(PublisherResult));
			return false;
		}
	}

	OutError.Reset();
	return true;
}

bool UFlecsNetworkSessionSubsystem::InitializeClientModel(FPeerSession& Session)
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (!World || World->GetNetMode() != NM_Client || !FlecsSubsystem ||
		!FrozenWorkerRegistry.IsValid() ||
		Session.ServerWorldEpoch == 0 || Session.LocalAttachmentEpoch != LocalAttachmentEpoch)
	{
		return false;
	}

	const uint64 ExpectedLocalEpoch = Session.LocalAttachmentEpoch;
	const uint64 ServerEpoch = Session.ServerWorldEpoch;
	const uint64 ConnectionIncarnation = Session.Incarnation.Value;
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const TSharedRef<FlecsModRegistry::FRegistry, ESPMode::ThreadSafe> RegistryRef = FrozenWorkerRegistry.ToSharedRef();
	const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(ExpectedLocalEpoch,
		[BridgeRef, RegistryRef, ExpectedLocalEpoch, ServerEpoch, ConnectionIncarnation](flecs::world& WorkerWorld)
		{
			if (BridgeRef->Model && BridgeRef->bModelInitialized)
			{
				if (BridgeRef->LocalAttachmentEpoch == ExpectedLocalEpoch && BridgeRef->ServerWorldEpoch == ServerEpoch &&
					BridgeRef->ClientModelIncarnation == ConnectionIncarnation && BridgeRef->Model->IsClientSessionUsable())
				{
					return;
				}
				BridgeRef->Model->Shutdown();
			}
			BridgeRef->Model = MakeUnique<FlecsSimulation::FWorkerEntityModel>(*RegistryRef, false);
			const FlecsSimulation::EModelResult Result = BridgeRef->Model->Initialize(
				WorkerWorld, ServerEpoch, FlecsSimulation::FWorkerEntityModel::FPermissionCheck());
			BridgeRef->bModelInitialized = Result == FlecsSimulation::EModelResult::Accepted;
			BridgeRef->LocalAttachmentEpoch = ExpectedLocalEpoch;
			BridgeRef->ServerWorldEpoch = ServerEpoch;
			BridgeRef->SimulationTick = 0;
			BridgeRef->LastClientSnapshotRevision = 0;
			BridgeRef->ClientModelIncarnation = BridgeRef->bModelInitialized ? ConnectionIncarnation : 0;
			{
				FScopeLock Lock(&BridgeRef->ResultMutex);
				BridgeRef->ClientApplyResults.Empty();
			}
			if (!BridgeRef->bModelInitialized)
			{
				BridgeRef->Model.Reset();
			}
		});
	if (QueueResult != EFlecsWorkerCommandResult::Accepted)
	{
		Session.RejectionReason = FString::Printf(TEXT("Could not queue client model initialization (result %u)."),
			static_cast<uint32>(QueueResult));
		return false;
	}
	if (PendingClientModelResetIncarnation != 0 && PendingClientModelResetIncarnation != ConnectionIncarnation)
	{
		// This accepted initialization is ordered on the worker and replaces any older client model.
		PendingClientModelResetIncarnation = 0;
		PendingClientModelResetLocalEpoch = 0;
	}
	return true;
}

bool UFlecsNetworkSessionSubsystem::IsPeerCompatible(
	FConnectionIncarnation Incarnation, uint64& OutServerWorldEpoch) const
{
	check(IsInGameThread());
	const FPeerSession* Session = FindSession(Incarnation);
	if (!Session || Session->State != EFlecsPeerSessionState::Compatible ||
		!Session->Channel.IsValid() || Session->ServerWorldEpoch == 0 ||
		!Session->SessionMapping.IsValid() || Session->LocalAttachmentEpoch == 0 ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch)
	{
		return false;
	}
	OutServerWorldEpoch = Session->ServerWorldEpoch;
	return true;
}

AFlecsReplicationChannel* UFlecsNetworkSessionSubsystem::GetCompatibleChannel(
	FConnectionIncarnation Incarnation) const
{
	uint64 IgnoredServerEpoch = 0;
	if (!IsPeerCompatible(Incarnation, IgnoredServerEpoch))
	{
		return nullptr;
	}
	const FPeerSession* Session = FindSession(Incarnation);
	return Session ? Session->Channel.Get() : nullptr;
}

bool UFlecsNetworkSessionSubsystem::GetPeerSessionMapping(
	FConnectionIncarnation Incarnation, FlecsModRegistry::FSessionMapping& OutMapping) const
{
	check(IsInGameThread());
	const FPeerSession* Session = FindSession(Incarnation);
	if (!Session || Session->State != EFlecsPeerSessionState::Compatible || !Session->SessionMapping.IsValid() ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch || Session->ServerWorldEpoch == 0)
	{
		return false;
	}
	OutMapping = Session->SessionMapping;
	return true;
}

void UFlecsNetworkSessionSubsystem::StartPendingServerHandshakes()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	if (bRetirementFailClosed || !bModRegistryFrozen || !World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		if (Session.State != EFlecsPeerSessionState::AwaitingManifest || Session.bHandshakeStarted || !IsValid(Channel))
		{
			continue;
		}
		if (Channel->StartManifestHandshake(ServerWorldEpoch, LocalManifestDescriptorBytes))
		{
			Session.ServerWorldEpoch = ServerWorldEpoch;
			Session.bHandshakeStarted = true;
			Session.bManifestHandshakeCompleted = false;
		}
	}
}

UFlecsNetworkSessionSubsystem::FPeerSession*
UFlecsNetworkSessionSubsystem::FindSessionByChannel(AFlecsReplicationChannel* Channel)
{
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (Pair.Value.Channel.Get() == Channel)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

void UFlecsNetworkSessionSubsystem::HandleSnapshotTransferFinished(
	AFlecsReplicationChannel* Channel, uint64 InServerWorldEpoch, uint64 TransferRevision, bool bSucceeded)
{
	check(IsInGameThread());
	(void)TransferRevision;
	FPeerSession* Session = FindSessionByChannel(Channel);
	if (!Session || Session->State != EFlecsPeerSessionState::Compatible ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch || Session->ServerWorldEpoch != InServerWorldEpoch ||
		!Session->Runtime.IsValid())
	{
		return;
	}
	FFlecsGenericPeerSchedulingRuntime& Runtime = *Session->Runtime;
	if (!Runtime.bTransferPending || Runtime.PendingSelection.SelectionId == 0 ||
		Runtime.PendingModelRevision == 0 || Runtime.PendingWireRecords.IsEmpty())
	{
		return;
	}
	if (!bSucceeded)
	{
		if (Runtime.TransferRetryCount >= 3)
		{
			Runtime.bTransferPending = false;
			RejectPeerManifest(Session->Incarnation, TEXT("The peer could not acknowledge the bounded state update after retries."));
			return;
		}
		if (!Channel->PublishUpdateBatch(Session->ServerWorldEpoch, Runtime.PendingWireRecords,
			Runtime.PendingSimulationTick))
		{
			Runtime.bTransferPending = false;
			const FString ChannelReason = Channel->GetLastUpdateBatchRejectionReason();
			FString Reason = TEXT("Iris refused the bounded state update retry without a diagnostic.");
			if (!ChannelReason.IsEmpty())
			{
				Reason = FString::Printf(TEXT("Iris refused the bounded state update retry: %s"), *ChannelReason);
			}
			RejectPeerManifest(Session->Incarnation, Reason.Left(512));
			return;
		}
		++Runtime.TransferRetryCount;
		return;
	}
	Runtime.bTransferPending = false;
	Runtime.bTransferSucceeded = true;
	Runtime.bModelAckPending = true;
	TryQueueDeltaAcknowledgement(*Session);
	EmitPeerEvent(*Session, EFlecsSessionEventKind::ServerTransferAcknowledged, TransferRevision);
}

void UFlecsNetworkSessionSubsystem::TryQueueDeltaAcknowledgement(FPeerSession& Session)
{
	check(IsInGameThread());
	if (!Session.Runtime.IsValid() || !Session.Runtime->bModelAckPending ||
		Session.Runtime->bModelAckQueued || !Session.Runtime->bTransferSucceeded || !WorkerBridgeState.IsValid())
	{
		return;
	}
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	if (!FlecsSubsystem)
	{
		return;
	}
	const uint64 LocalEpoch = Session.LocalAttachmentEpoch;
	const uint64 ServerEpoch = Session.ServerWorldEpoch;
	const uint64 ConnectionIncarnation = Session.Incarnation.Value;
	const uint64 ModelPublicationRevision = Session.Runtime->PendingModelRevision;
	const uint64 SelectionId = Session.Runtime->PendingSelection.SelectionId;
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
		[BridgeRef, LocalEpoch, ServerEpoch, ConnectionIncarnation, ModelPublicationRevision, SelectionId](flecs::world&)
		{
			FFlecsGenericWorkerBridgeState::FDeltaAckResult Result;
			Result.LocalAttachmentEpoch = LocalEpoch;
			Result.ServerWorldEpoch = ServerEpoch;
			Result.ModelPublicationRevision = ModelPublicationRevision;
			Result.SelectionId = SelectionId;
			FScopeLock Lock(&BridgeRef->ResultMutex);
			if (BridgeRef->bRetirementFailClosed || BridgeRef->RetiredConnectionIncarnations.Contains(ConnectionIncarnation))
			{
				return;
			}
			if (BridgeRef->LocalAttachmentEpoch == LocalEpoch && BridgeRef->ServerWorldEpoch == ServerEpoch &&
				BridgeRef->Model && BridgeRef->bModelInitialized)
			{
				Result.Result = BridgeRef->Model->AcknowledgeAuthoritativeDelta(
					ConnectionIncarnation, ServerEpoch, ModelPublicationRevision);
			}
			BridgeRef->DeltaAckResults.Add(ConnectionIncarnation, Result);
		});
	if (QueueResult == EFlecsWorkerCommandResult::Accepted)
	{
		Session.Runtime->bModelAckQueued = true;
	}
}

void UFlecsNetworkSessionSubsystem::BindPeerChannel(FPeerSession& Session)
{
	check(IsInGameThread());
	AFlecsReplicationChannel* Channel = Session.Channel.Get();
	if (!IsValid(Channel))
	{
		return;
	}
	const TWeakObjectPtr<UFlecsNetworkSessionSubsystem> WeakThis(this);
	const TWeakObjectPtr<AFlecsReplicationChannel> WeakChannel(Channel);
	Channel->OnDiscreteCommandAdmission = [WeakThis, WeakChannel](const FFlecsCommandEnvelope& Envelope)
	{
		UFlecsNetworkSessionSubsystem* Subsystem = WeakThis.Get();
		AFlecsReplicationChannel* ActiveChannel = WeakChannel.Get();
		return Subsystem && IsValid(ActiveChannel) && Subsystem->AdmitDiscreteCommand(ActiveChannel, Envelope);
	};
	Channel->OnContinuousCommandReceived.AddLambda([WeakThis, WeakChannel](const FFlecsCommandEnvelope& Envelope)
	{
		if (UFlecsNetworkSessionSubsystem* Subsystem = WeakThis.Get())
		{
			if (AFlecsReplicationChannel* ActiveChannel = WeakChannel.Get())
			{
				Subsystem->QueueContinuousCommand(ActiveChannel, Envelope);
			}
		}
	});
	Channel->OnDiscreteCommandResultReceived.AddLambda([WeakThis, WeakChannel](uint32 Sequence, uint8 ResultCode)
	{
		if (UFlecsNetworkSessionSubsystem* Subsystem = WeakThis.Get())
		{
			if (FPeerSession* Peer = Subsystem->FindSessionByChannel(WeakChannel.Get()))
			{
				Subsystem->EmitPeerEvent(*Peer, EFlecsSessionEventKind::ClientCommandResult, 0, 0, Sequence, ResultCode);
			}
		}
	});
	Channel->OnCompleteStateBatchReceived.AddLambda([WeakThis, WeakChannel](uint64 SnapshotServerWorldEpoch,
		uint64 SnapshotRevision, uint32 SimulationTick, const TArray<FFlecsReplicationRecord>& Records)
	{
		if (UFlecsNetworkSessionSubsystem* Subsystem = WeakThis.Get())
		{
			if (AFlecsReplicationChannel* ActiveChannel = WeakChannel.Get())
			{
				Subsystem->HandleCompleteClientStateBatch(ActiveChannel, SnapshotServerWorldEpoch,
					SnapshotRevision, SimulationTick, Records);
			}
		}
	});
	Channel->OnSnapshotTransferFinished.AddLambda([WeakThis, WeakChannel](uint64 ServerWorldEpoch,
		uint64 TransferRevision, bool bSucceeded)
	{
		if (UFlecsNetworkSessionSubsystem* Subsystem = WeakThis.Get())
		{
			if (AFlecsReplicationChannel* ActiveChannel = WeakChannel.Get())
			{
				Subsystem->HandleSnapshotTransferFinished(ActiveChannel, ServerWorldEpoch,
					TransferRevision, bSucceeded);
			}
		}
	});
}

void UFlecsNetworkSessionSubsystem::HandleCompleteClientStateBatch(
	AFlecsReplicationChannel* Channel, uint64 SnapshotServerWorldEpoch, uint64 SnapshotRevision,
	uint32 SimulationTick, const TArray<FFlecsReplicationRecord>& Records)
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	FPeerSession* Session = FindSessionByChannel(Channel);
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	FlecsModRegistry::FSessionMapping Mapping;
	if (!World || World->GetNetMode() != NM_Client || !Session || !FlecsSubsystem ||
		Session->State != EFlecsPeerSessionState::Compatible ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch ||
		SnapshotServerWorldEpoch == 0 || SnapshotServerWorldEpoch != Session->ServerWorldEpoch ||
		SnapshotRevision == 0 || SnapshotRevision <= Session->LastQueuedSnapshotRevision ||
		SimulationTick == 0 || !GetPeerSessionMapping(Session->Incarnation, Mapping) ||
		!WorkerBridgeState.IsValid())
	{
		if (IsValid(Channel) && SnapshotServerWorldEpoch != 0 && SnapshotRevision != 0)
		{
			Channel->CompleteReceivedSnapshotBatch(SnapshotServerWorldEpoch, SnapshotRevision, false);
		}
		return;
	}

	FlecsSimulation::FStateBatch Batch;
	Batch.ServerWorldEpoch = SnapshotServerWorldEpoch;
	Batch.SimulationTick = SimulationTick;
	Batch.bCompleteStateSnapshot = false;
	Batch.Records.Reserve(Records.Num());
	for (const FFlecsReplicationRecord& WireRecord : Records)
	{
		if (WireRecord.WorldEpoch != SnapshotServerWorldEpoch || WireRecord.SnapshotRevision != SnapshotRevision ||
			WireRecord.SnapshotSimulationTick != SimulationTick || WireRecord.Operation > 4)
		{
			Channel->CompleteReceivedSnapshotBatch(SnapshotServerWorldEpoch, SnapshotRevision, false);
			RejectPeerManifest(Session->Incarnation, TEXT("The committed snapshot contains inconsistent record metadata."));
			return;
		}
		FlecsSimulation::FStateRecord& StateRecord = Batch.Records.AddDefaulted_GetRef();
		StateRecord.Identity.ServerWorldEpoch = WireRecord.WorldEpoch;
		StateRecord.Identity.NetworkEntityId = WireRecord.NetworkEntityId;
		StateRecord.Identity.Generation = WireRecord.EntityGeneration;
		StateRecord.DefinitionWireId = WireRecord.DefinitionWireId;
		StateRecord.ComponentWireId = WireRecord.ComponentWireId;
		StateRecord.SourceSimulationTick = WireRecord.SourceSimulationTick;
		StateRecord.Payload = WireRecord.Payload;
		switch (WireRecord.Operation)
		{
		case 0: StateRecord.Operation = FlecsSimulation::EStateOperation::ComponentUpsert; break;
		case 1: StateRecord.Operation = FlecsSimulation::EStateOperation::ComponentRemove; break;
		case 2: StateRecord.Operation = FlecsSimulation::EStateOperation::EntityDespawn; break;
		case 3: StateRecord.Operation = FlecsSimulation::EStateOperation::EntityCreate; break;
		case 4: StateRecord.Operation = FlecsSimulation::EStateOperation::EntityLeaveRelevance; break;
		default: break;
		}
	}

	const uint64 LocalEpoch = Session->LocalAttachmentEpoch;
	const uint64 ServerEpoch = Session->ServerWorldEpoch;
	const uint64 Incarnation = Session->Incarnation.Value;
	FString InspectionComponent;
#if WITH_DEV_AUTOMATION_TESTS
	InspectionComponent = ClientInspectionComponent;
#endif
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const EFlecsWorkerCommandResult QueueResult = FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
		[BridgeRef, Batch = MoveTemp(Batch), Mapping, LocalEpoch, ServerEpoch, Incarnation, SnapshotRevision, SimulationTick, InspectionComponent]
		(flecs::world&) mutable
	{
		(void)InspectionComponent;
		FFlecsGenericWorkerBridgeState::FClientApplyResult ApplyResult;
		ApplyResult.LocalAttachmentEpoch = LocalEpoch;
		ApplyResult.ServerWorldEpoch = ServerEpoch;
		ApplyResult.ConnectionIncarnation = Incarnation;
		ApplyResult.SnapshotRevision = SnapshotRevision;
		ApplyResult.SimulationTick = SimulationTick;
		if (BridgeRef->LocalAttachmentEpoch == LocalEpoch && BridgeRef->ServerWorldEpoch == ServerEpoch &&
			BridgeRef->ClientModelIncarnation == Incarnation &&
			BridgeRef->Model && BridgeRef->bModelInitialized && SnapshotRevision > BridgeRef->LastClientSnapshotRevision)
		{
			ApplyResult.Result = BridgeRef->Model->ApplyClientBatch(Batch, Mapping);
			if (ApplyResult.Result == FlecsSimulation::EModelResult::Accepted)
			{
				BridgeRef->LastClientSnapshotRevision = SnapshotRevision;
#if WITH_DEV_AUTOMATION_TESTS
				if (!InspectionComponent.IsEmpty())
				{
					for (const FlecsSimulation::FStateRecord& Record : Batch.Records)
					{
						if (Record.Operation != FlecsSimulation::EStateOperation::ComponentUpsert) continue;
						FlecsModRegistry::FSessionWireEntry Definition;
						FlecsModRegistry::FSessionWireEntry Component;
						if (!Mapping.FindEntryByWireId(Record.DefinitionWireId, Definition) ||
							Definition.Kind != FlecsModRegistry::EEntryKind::EntityDefinition ||
							!Mapping.FindEntryByWireId(Record.ComponentWireId, Component) ||
							Component.Kind != FlecsModRegistry::EEntryKind::Component || Component.StableId != InspectionComponent) continue;
						FlecsSimulation::FNetworkEntityIdentity Identity = Record.Identity;
						Identity.DefinitionStableId = Definition.StableId;
						TArray<uint8> Bytes;
						if (BridgeRef->Model->InspectComponentBytesForTesting(Identity, InspectionComponent, Bytes) ==
							FlecsSimulation::EModelResult::Accepted && Bytes.Num() <= 1024)
						{
							ApplyResult.InspectedIdentity = MoveTemp(Identity);
							ApplyResult.InspectedBytes = MoveTemp(Bytes);
							break;
						}
					}
				}
#endif
			}
		}
		FScopeLock Lock(&BridgeRef->ResultMutex);
		BridgeRef->ClientApplyResults.Add(Incarnation, ApplyResult);
	});
	if (QueueResult != EFlecsWorkerCommandResult::Accepted)
	{
		Channel->CompleteReceivedSnapshotBatch(SnapshotServerWorldEpoch, SnapshotRevision, false);
		RejectPeerManifest(Session->Incarnation,
			FString::Printf(TEXT("Could not queue committed snapshot for worker apply (result %u)."),
				static_cast<uint32>(QueueResult)));
		return;
	}
	Session->LastQueuedSnapshotRevision = SnapshotRevision;
}

void UFlecsNetworkSessionSubsystem::ProcessClientReplication()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_Client || !WorkerBridgeState.IsValid())
	{
		return;
	}
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		if (Session.State != EFlecsPeerSessionState::Compatible || !Session.Incarnation.IsValid())
		{
			continue;
		}
		FFlecsGenericWorkerBridgeState::FClientApplyResult ApplyResult;
		bool bHasResult = false;
		{
			FScopeLock Lock(&WorkerBridgeState->ResultMutex);
			if (FFlecsGenericWorkerBridgeState::FClientApplyResult* Found =
				WorkerBridgeState->ClientApplyResults.Find(Session.Incarnation.Value))
			{
				ApplyResult = *Found;
				WorkerBridgeState->ClientApplyResults.Remove(Session.Incarnation.Value);
				bHasResult = true;
			}
		}
		if (!bHasResult)
		{
			continue;
		}
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		const bool bCurrent = ApplyResult.SnapshotRevision == Session.LastQueuedSnapshotRevision &&
			ApplyResult.LocalAttachmentEpoch == Session.LocalAttachmentEpoch &&
			ApplyResult.ConnectionIncarnation == Session.Incarnation.Value &&
			ApplyResult.ServerWorldEpoch == Session.ServerWorldEpoch && IsValid(Channel);
		if (!bCurrent)
		{
			if (IsValid(Channel))
			{
				Channel->CompleteReceivedSnapshotBatch(ApplyResult.ServerWorldEpoch, ApplyResult.SnapshotRevision, false);
			}
			RejectPeerManifest(Session.Incarnation, TEXT("Worker snapshot result belongs to a stale session or revision."));
			continue;
		}
		if (ApplyResult.Result != FlecsSimulation::EModelResult::Accepted)
		{
			Channel->CompleteReceivedSnapshotBatch(ApplyResult.ServerWorldEpoch, ApplyResult.SnapshotRevision, false);
			RejectPeerManifest(Session.Incarnation,
				FString::Printf(TEXT("Worker rejected complete snapshot revision %llu (result %u)."),
					static_cast<unsigned long long>(ApplyResult.SnapshotRevision),
					static_cast<uint32>(ApplyResult.Result)));
			continue;
		}
		if (!Channel->CompleteReceivedSnapshotBatch(ApplyResult.ServerWorldEpoch, ApplyResult.SnapshotRevision, true))
		{
			RejectPeerManifest(Session.Incarnation, TEXT("Could not acknowledge the successfully applied client batch."));
			continue;
		}
#if WITH_DEV_AUTOMATION_TESTS
		EmitPeerEvent(Session, EFlecsSessionEventKind::ClientStateApplied,
			ApplyResult.SnapshotRevision, ApplyResult.SimulationTick, 0, 0, FString(),
			&ApplyResult.InspectedIdentity, &ApplyResult.InspectedBytes);
#else
		EmitPeerEvent(Session, EFlecsSessionEventKind::ClientStateApplied,
			ApplyResult.SnapshotRevision, ApplyResult.SimulationTick);
#endif
	}
}

bool UFlecsNetworkSessionSubsystem::AdmitDiscreteCommand(
	AFlecsReplicationChannel* Channel, const FFlecsCommandEnvelope& Envelope)
{
	check(IsInGameThread());
	if (bRetirementFailClosed) return false;
	FPeerSession* Session = FindSessionByChannel(Channel);
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	FlecsModRegistry::FSessionMapping Mapping;
	if (!Session || !FlecsSubsystem || Session->State != EFlecsPeerSessionState::Compatible ||
		!Session->Connection.IsValid() || !Session->Controller.IsValid() || Channel->GetOwner() != Session->Controller.Get() ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch || Envelope.WorldEpoch != Session->ServerWorldEpoch ||
		!GetPeerSessionMapping(Session->Incarnation, Mapping) || !WorkerBridgeState.IsValid())
	{
		return false;
	}

	FlecsSimulation::FClientCommand Command;
	Command.ServerWorldEpoch = Envelope.WorldEpoch;
	Command.TrustedConnectionToken = Session->Incarnation.Value;
	Command.Sequence = Envelope.Sequence;
	Command.TargetNetworkEntityId = Envelope.TargetNetworkEntityId;
	Command.TargetGeneration = Envelope.TargetEntityGeneration;
	Command.RequestedSimulationTick = Envelope.RequestedSimulationTick;
	Command.CommandWireId = Envelope.CommandWireId;
	Command.bDiscrete = true;
	Command.Payload = Envelope.Payload;
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const uint64 LocalEpoch = Session->LocalAttachmentEpoch;
	const uint64 ConnectionIncarnation = Session->Incarnation.Value;
	const EFlecsWorkerCommandResult Result = FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
		[BridgeRef, Mapping, Command = MoveTemp(Command), ConnectionIncarnation](flecs::world&) mutable
		{
			FScopeLock Lock(&BridgeRef->ResultMutex);
			if (BridgeRef->bRetirementFailClosed || BridgeRef->RetiredConnectionIncarnations.Contains(ConnectionIncarnation))
			{
				return;
			}
			FlecsSimulation::EModelResult ExecutionResult = FlecsSimulation::EModelResult::CallbackFailed;
			if (BridgeRef->Model && BridgeRef->bModelInitialized)
			{
				ExecutionResult = BridgeRef->Model->ExecuteAuthoritativeCommand(Command, BridgeRef->SimulationTick, Mapping);
			}
			TMap<uint32, FFlecsGenericWorkerBridgeState::FCommandExecutionResult>& Results =
				BridgeRef->CommandExecutionResults.FindOrAdd(ConnectionIncarnation);
			if (Results.Num() >= 32 && !Results.Contains(Command.Sequence))
			{
				BridgeRef->CommandResultOverflowIncarnations.Add(ConnectionIncarnation);
			}
			else
			{
				FFlecsGenericWorkerBridgeState::FCommandExecutionResult& Result = Results.FindOrAdd(Command.Sequence);
				Result.ServerWorldEpoch = Command.ServerWorldEpoch;
				Result.Sequence = Command.Sequence;
				Result.Result = ExecutionResult;
			}
		});
	return Result == EFlecsWorkerCommandResult::Accepted;
}

void UFlecsNetworkSessionSubsystem::QueueContinuousCommand(
	AFlecsReplicationChannel* Channel, const FFlecsCommandEnvelope& Envelope)
{
	check(IsInGameThread());
	if (bRetirementFailClosed) return;
	FPeerSession* Session = FindSessionByChannel(Channel);
	UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get();
	FlecsModRegistry::FSessionMapping Mapping;
	if (!Session || !FlecsSubsystem || Session->State != EFlecsPeerSessionState::Compatible ||
		!Session->Connection.IsValid() || !Session->Controller.IsValid() || Channel->GetOwner() != Session->Controller.Get() ||
		Session->LocalAttachmentEpoch != LocalAttachmentEpoch || Envelope.WorldEpoch != Session->ServerWorldEpoch ||
		!GetPeerSessionMapping(Session->Incarnation, Mapping) || !WorkerBridgeState.IsValid())
	{
		return;
	}
	FlecsSimulation::FClientCommand Command;
	Command.ServerWorldEpoch = Envelope.WorldEpoch;
	Command.TrustedConnectionToken = Session->Incarnation.Value;
	Command.Sequence = Envelope.Sequence;
	Command.TargetNetworkEntityId = Envelope.TargetNetworkEntityId;
	Command.TargetGeneration = Envelope.TargetEntityGeneration;
	Command.RequestedSimulationTick = Envelope.RequestedSimulationTick;
	Command.CommandWireId = Envelope.CommandWireId;
	Command.bDiscrete = false;
	Command.Payload = Envelope.Payload;
	const TSharedRef<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> BridgeRef = WorkerBridgeState.ToSharedRef();
	const uint64 LocalEpoch = Session->LocalAttachmentEpoch;
	const uint64 ConnectionIncarnation = Session->Incarnation.Value;
	FlecsSubsystem->SubmitWorkerCommand(LocalEpoch,
		[BridgeRef, Mapping, Command = MoveTemp(Command), ConnectionIncarnation](flecs::world&) mutable
		{
			FScopeLock Lock(&BridgeRef->ResultMutex);
			if (BridgeRef->bRetirementFailClosed || BridgeRef->RetiredConnectionIncarnations.Contains(ConnectionIncarnation))
			{
				return;
			}
			if (BridgeRef->Model && BridgeRef->bModelInitialized)
			{
				BridgeRef->Model->ExecuteAuthoritativeCommand(Command, BridgeRef->SimulationTick, Mapping);
			}
		});
}

void UFlecsNetworkSessionSubsystem::HandleManifestDescriptor(
	AFlecsReplicationChannel* Channel, uint64 InServerWorldEpoch, const TArray<uint8>& Bytes)
{
	check(IsInGameThread());
	FPeerSession* Session = FindSessionByChannel(Channel);
	UWorld* World = GetWorld();
	if (!Session || !World || !bModRegistryFrozen || Session->LocalAttachmentEpoch != LocalAttachmentEpoch ||
		Session->State != EFlecsPeerSessionState::AwaitingManifest || InServerWorldEpoch == 0 ||
		Bytes.IsEmpty() || Bytes.Num() > FlecsModRegistry::MaxManifestDescriptorBytes)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		Session->ServerWorldEpoch = InServerWorldEpoch;
	}
	FlecsModRegistry::FManifestDescriptor RemoteDescriptor;
	FString Error;
	if (!FlecsModRegistry::FManifestDescriptor::Deserialize(Bytes, RemoteDescriptor, Error))
	{
		RejectPeerManifest(Session->Incarnation, Error);
		return;
	}
	if (!ValidateManifestTransportLimits(RemoteDescriptor, Error))
	{
		RejectPeerManifest(Session->Incarnation, Error);
		return;
	}
	const FlecsModRegistry::FCompatibilityResult Result = ModRegistry.CompareRequired(RemoteDescriptor);
	if (!Result.bCompatible || !Result.SessionMapping.IsValid())
	{
		FString Reason;
		for (const FString& Diagnostic : Result.Diagnostics)
		{
			if (!Reason.IsEmpty()) Reason += TEXT("; ");
			Reason += Diagnostic;
		}
		RejectPeerManifest(Session->Incarnation, Reason.IsEmpty() ? TEXT("Manifest compatibility failed.") : Reason);
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		Session->ServerWorldEpoch = InServerWorldEpoch;
		Session->SessionMapping = Result.SessionMapping;
		if (!Channel->SubmitLocalManifestDescriptor(InServerWorldEpoch, LocalManifestDescriptorBytes))
		{
			RejectPeerManifest(Session->Incarnation, TEXT("Could not submit the local manifest descriptor."));
		}
		return;
	}
	if (InServerWorldEpoch != ServerWorldEpoch || !Session->bHandshakeStarted)
	{
		RejectPeerManifest(Session->Incarnation, TEXT("Manifest belongs to a stale server world epoch."));
		return;
	}
	Session->ServerWorldEpoch = InServerWorldEpoch;
	Session->SessionMapping = Result.SessionMapping;
	Session->State = EFlecsPeerSessionState::Compatible;
	Session->RejectionReason.Reset();
	if (!Channel->CompleteServerManifestHandshake(InServerWorldEpoch, true, FString()))
	{
		Session->State = EFlecsPeerSessionState::Rejected;
		Session->SessionMapping = FlecsModRegistry::FSessionMapping();
		Session->RejectionReason = TEXT("Could not complete the server manifest handshake.");
	}
}

void UFlecsNetworkSessionSubsystem::HandleManifestHandshakeFinished(
	AFlecsReplicationChannel* Channel, uint64 InServerWorldEpoch, const FString& Reason)
{
	check(IsInGameThread());
	FPeerSession* Session = FindSessionByChannel(Channel);
	if (!Session || Session->LocalAttachmentEpoch != LocalAttachmentEpoch || InServerWorldEpoch == 0)
	{
		return;
	}
	if (!Reason.IsEmpty())
	{
		const bool bNewRejection = Session->State != EFlecsPeerSessionState::Rejected;
		if (bNewRejection)
		{
			Session->State = EFlecsPeerSessionState::Rejected;
			EmitPeerEvent(*Session, EFlecsSessionEventKind::Rejected, 0, 0, 0, 0, Reason.Left(512));
			if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
			{
				QueueClientModelReset(Session->Incarnation, Session->LocalAttachmentEpoch);
			}
			else
			{
				RetireConnectionIncarnation(Session->Incarnation, InServerWorldEpoch);
			}
		}
		Session->State = EFlecsPeerSessionState::Rejected;
		Session->ServerWorldEpoch = 0;
		Session->SessionMapping = FlecsModRegistry::FSessionMapping();
		Session->RejectionReason = Reason.Left(512);
		return;
	}
	if (!Session->SessionMapping.IsValid())
	{
		Session->State = EFlecsPeerSessionState::Rejected;
		Session->RejectionReason = TEXT("Handshake completed without a validated session mapping.");
		return;
	}
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		Session->ServerWorldEpoch = InServerWorldEpoch;
	}
	else if (InServerWorldEpoch != ServerWorldEpoch)
	{
		Session->State = EFlecsPeerSessionState::Rejected;
		Session->RejectionReason = TEXT("Handshake result belongs to a stale server epoch.");
		return;
	}
	Session->State = EFlecsPeerSessionState::Compatible;
	Session->RejectionReason.Reset();
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client && !InitializeClientModel(*Session))
	{
		Session->State = EFlecsPeerSessionState::Rejected;
		Session->SessionMapping = FlecsModRegistry::FSessionMapping();
		Channel->RejectManifestHandshake(InServerWorldEpoch, Session->RejectionReason);
		return;
	}
	Session->bManifestHandshakeCompleted = true;
	EmitPeerEvent(*Session, EFlecsSessionEventKind::Compatible);
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		Channel->ResumeReceivedState();
	}
}

void UFlecsNetworkSessionSubsystem::ProcessPendingChannelHandshakes()
{
	check(IsInGameThread());
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		FPeerSession& Session = Pair.Value;
		AFlecsReplicationChannel* Channel = Session.Channel.Get();
		if (!IsValid(Channel))
		{
			continue;
		}
		uint64 EventEpoch = 0;
		TArray<uint8> Descriptor;
		if (Channel->ConsumePendingManifestDescriptor(EventEpoch, Descriptor))
		{
			HandleManifestDescriptor(Channel, EventEpoch, Descriptor);
		}
		bool bCompatible = false;
		FString Reason;
		if (Channel->ConsumePendingManifestResult(EventEpoch, bCompatible, Reason))
		{
			HandleManifestHandshakeFinished(Channel, EventEpoch, bCompatible ? FString() : Reason);
		}
	}
}

bool UFlecsNetworkSessionSubsystem::RejectPeerManifest(
	FConnectionIncarnation Incarnation, const FString& Reason)
{
	check(IsInGameThread());
	FPeerSession* Session = FindSession(Incarnation);
	if (!Session || Session->State == EFlecsPeerSessionState::Closing)
	{
		return false;
	}
	const uint64 RejectedEpoch = Session->ServerWorldEpoch != 0 ? Session->ServerWorldEpoch : ServerWorldEpoch;
	if (Session->State != EFlecsPeerSessionState::Rejected)
	{
		Session->State = EFlecsPeerSessionState::Rejected;
		EmitPeerEvent(*Session, EFlecsSessionEventKind::Rejected, 0, 0, 0, 0, Reason.Left(512));
		if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
		{
			QueueClientModelReset(Incarnation, Session->LocalAttachmentEpoch);
		}
		else
		{
			RetireConnectionIncarnation(Incarnation, RejectedEpoch);
		}
	}
	Session->State = EFlecsPeerSessionState::Rejected;
	Session->RejectionReason = Reason.Left(512);
	Session->ServerWorldEpoch = 0;
	Session->SessionMapping = FlecsModRegistry::FSessionMapping();
	if (AFlecsReplicationChannel* Channel = Session->Channel.Get())
	{
		Channel->RejectManifestHandshake(RejectedEpoch, Session->RejectionReason);
	}
	return true;
}

void UFlecsNetworkSessionSubsystem::ReconcileConnections()
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	if (bIsClosing || !World)
	{
		return;
	}
	const uint64 PreviousLocalAttachmentEpoch = LocalAttachmentEpoch;
	if (UFlecsGameInstanceSubsystem* FlecsSubsystem = FlecsGameInstanceSubsystem.Get())
	{
		LocalAttachmentEpoch = FlecsSubsystem->GetActiveEpoch();
	}
	if (LocalAttachmentEpoch != 0 && LocalAttachmentEpoch != PreviousLocalAttachmentEpoch && WorkerBridgeState.IsValid())
	{
		bRetirementFailClosed = false;
		FScopeLock Lock(&WorkerBridgeState->ResultMutex);
		WorkerBridgeState->bRetirementFailClosed = false;
	}
	if (LocalAttachmentEpoch == 0)
	{
		CloseAllSessions(World->GetNetMode() != NM_Client);
		return;
	}
	if (bRetirementFailClosed)
	{
		CloseAllSessions(true);
		return;
	}
	bool bSessionEpochChanged = false;
	for (const TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (Pair.Value.LocalAttachmentEpoch != LocalAttachmentEpoch)
		{
			bSessionEpochChanged = true;
			break;
		}
	}
	if (bSessionEpochChanged)
	{
		CloseAllSessions(World->GetNetMode() != NM_Client);
	}
	if (World->GetNetMode() == NM_Client)
	{
		ReconcileClientChannels(*World);
	}
	else if (World->GetNetMode() == NM_ListenServer || World->GetNetMode() == NM_DedicatedServer)
	{
		ReconcileServerConnections(*World);
	}
	else
	{
		CloseAllSessions(false);
	}
	StartPendingServerHandshakes();
	ProcessPendingChannelHandshakes();
}

void UFlecsNetworkSessionSubsystem::ReconcileServerConnections(UWorld& World)
{
	if (!bModRegistryFrozen)
	{
		return;
	}
	TSet<TWeakObjectPtr<APlayerController>> PresentControllers;
	for (FConstPlayerControllerIterator It = World.GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* Controller = It->Get();
		if (!IsValid(Controller) || Controller->IsLocalController())
		{
			continue;
		}
		UNetConnection* Connection = Controller->GetNetConnection();
		if (!IsValid(Connection))
		{
			continue;
		}

		const TWeakObjectPtr<APlayerController> ControllerKey(Controller);
		PresentControllers.Add(ControllerKey);
		FPeerSession* Session = PeerSessions.Find(ControllerKey);
		if (Session && (Session->Connection.Get() != Connection || !Session->Channel.IsValid() ||
			Session->Channel->GetOwner() != Controller))
		{
			CloseSession(ControllerKey, true);
			Session = nullptr;
		}
		if (Session)
		{
			continue;
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Owner = Controller;
		SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AFlecsReplicationChannel* Channel = World.SpawnActor<AFlecsReplicationChannel>(SpawnParameters);
		if (!IsValid(Channel))
		{
			continue;
		}
		Channel->SetChannelOwner(Controller);
		if (Channel->GetOwner() != Controller)
		{
			Channel->Destroy();
			continue;
		}

		FPeerSession NewSession;
		NewSession.Incarnation = AllocateIncarnation();
		NewSession.Runtime = MakeShared<FFlecsGenericPeerSchedulingRuntime, ESPMode::ThreadSafe>();
		NewSession.State = EFlecsPeerSessionState::AwaitingManifest;
		NewSession.Controller = Controller;
		NewSession.Connection = Connection;
		NewSession.Channel = Channel;
		NewSession.LocalAttachmentEpoch = LocalAttachmentEpoch;
		NewSession.ServerWorldEpoch = ServerWorldEpoch;
		FPeerSession& AddedSession = PeerSessions.Add(ControllerKey, MoveTemp(NewSession));
		BindPeerChannel(AddedSession);
	}

	TArray<TWeakObjectPtr<APlayerController>> DepartedControllers;
	for (const TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (!PresentControllers.Contains(Pair.Key))
		{
			DepartedControllers.Add(Pair.Key);
		}
	}
	for (const TWeakObjectPtr<APlayerController>& Controller : DepartedControllers)
	{
		CloseSession(Controller, true);
	}
}

void UFlecsNetworkSessionSubsystem::ReconcileClientChannels(UWorld& World)
{
	TSet<TWeakObjectPtr<APlayerController>> PresentControllers;
	TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AFlecsReplicationChannel>> DiscoveredChannels;
	for (TActorIterator<AFlecsReplicationChannel> It(&World); It; ++It)
	{
		AFlecsReplicationChannel* Channel = *It;
		APlayerController* Controller = Cast<APlayerController>(Channel->GetOwner());
		if (!IsValid(Controller) || !Controller->IsLocalController())
		{
			continue;
		}
		const TWeakObjectPtr<APlayerController> ControllerKey(Controller);
		PresentControllers.Add(ControllerKey);
		const FPeerSession* ExistingSession = PeerSessions.Find(ControllerKey);
		const TWeakObjectPtr<AFlecsReplicationChannel>* Discovered = DiscoveredChannels.Find(ControllerKey);
		if (!Discovered || (ExistingSession && ExistingSession->Channel.Get() == Channel))
		{
			// Keep the current channel while an old and replacement actor overlap;
			// switch once it disappears instead of depending on iterator order.
			DiscoveredChannels.Add(ControllerKey, Channel);
		}
	}

	for (const TPair<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AFlecsReplicationChannel>>& Pair : DiscoveredChannels)
	{
		FPeerSession* Session = PeerSessions.Find(Pair.Key);
		if (Session && Session->Channel == Pair.Value)
		{
			continue;
		}
		if (Session)
		{
			CloseSession(Pair.Key, false);
		}
		FPeerSession NewSession;
		NewSession.Incarnation = AllocateIncarnation();
		NewSession.Runtime = MakeShared<FFlecsGenericPeerSchedulingRuntime, ESPMode::ThreadSafe>();
		NewSession.State = EFlecsPeerSessionState::AwaitingManifest;
		NewSession.Controller = Pair.Key;
		NewSession.Channel = Pair.Value;
		NewSession.LocalAttachmentEpoch = LocalAttachmentEpoch;
		// The client's local worker epoch is never used as the server network epoch.
		NewSession.ServerWorldEpoch = 0;
		FPeerSession& AddedSession = PeerSessions.Add(Pair.Key, MoveTemp(NewSession));
		BindPeerChannel(AddedSession);
	}

	TArray<TWeakObjectPtr<APlayerController>> DepartedControllers;
	for (const TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (!PresentControllers.Contains(Pair.Key))
		{
			DepartedControllers.Add(Pair.Key);
		}
	}
	for (const TWeakObjectPtr<APlayerController>& Controller : DepartedControllers)
	{
		CloseSession(Controller, false);
	}
}

void UFlecsNetworkSessionSubsystem::CloseSession(
	const TWeakObjectPtr<APlayerController>& Controller, bool bDestroyChannel)
{
	FPeerSession* Session = PeerSessions.Find(Controller);
	if (!Session)
	{
		return;
	}
	Session->State = EFlecsPeerSessionState::Closing;
	if (UWorld* World = GetWorld(); World && World->GetNetMode() == NM_Client)
	{
		QueueClientModelReset(Session->Incarnation, Session->LocalAttachmentEpoch);
	}
	else
	{
		RetireConnectionIncarnation(Session->Incarnation,
			Session->ServerWorldEpoch != 0 ? Session->ServerWorldEpoch : ServerWorldEpoch);
	}
	if (bDestroyChannel)
	{
		if (AFlecsReplicationChannel* Channel = Session->Channel.Get())
		{
			Channel->Destroy();
		}
	}
	PeerSessions.Remove(Controller);
}

void UFlecsNetworkSessionSubsystem::EmitPeerEvent(const FPeerSession& Session, EFlecsSessionEventKind Kind,
	uint64 Revision, uint32 SimulationTick, uint32 Sequence, uint8 Result, const FString& Reason,
	const FlecsSimulation::FNetworkEntityIdentity* InspectedIdentity, const TArray<uint8>* InspectedBytes)
{
	check(IsInGameThread());
	if (!OnSessionEvent.IsBound()) return;
	FFlecsSessionEvent Event;
	Event.Kind = Kind;
	Event.Incarnation = Session.Incarnation;
	Event.Controller = Session.Controller;
	Event.Channel = Session.Channel;
	Event.ServerWorldEpoch = Session.ServerWorldEpoch;
	Event.SnapshotRevision = Revision;
	Event.SimulationTick = SimulationTick;
	Event.CommandSequence = Sequence;
	Event.CommandResult = Result;
	Event.Reason = Reason.Left(512);
	if (InspectedIdentity && InspectedBytes && InspectedBytes->Num() <= 1024)
	{
		Event.InspectedIdentity = *InspectedIdentity;
		Event.InspectedComponentBytes = *InspectedBytes;
	}
	OnSessionEvent.Broadcast(Event);
}

#if WITH_DEV_AUTOMATION_TESTS
bool UFlecsNetworkSessionSubsystem::SetClientInspectionComponent(const FString& StableComponentId)
{
	check(IsInGameThread());
	if (bIsClosing || bModRegistryFrozen || !ModRegistry.FindComponent(StableComponentId)) return false;
	ClientInspectionComponent = StableComponentId;
	return true;
}
#endif

bool UFlecsNetworkSessionSubsystem::RequestChannelRecreation(FConnectionIncarnation Incarnation)
{
	check(IsInGameThread());
	const UWorld* World = GetWorld();
	const FPeerSession* Session = FindSession(Incarnation);
	if (bIsClosing || !World || World->GetNetMode() == NM_Client || !Session ||
		Session->State != EFlecsPeerSessionState::Compatible || !Session->Controller.IsValid()) return false;
	PendingChannelRecreations.Add(Incarnation.Value);
	return true;
}

void UFlecsNetworkSessionSubsystem::ProcessRequestedChannelRecreations()
{
	check(IsInGameThread());
	TSet<uint64> Requests = MoveTemp(PendingChannelRecreations);
	PendingChannelRecreations.Reset();
	for (uint64 Value : Requests)
	{
		FConnectionIncarnation Incarnation;
		Incarnation.Value = Value;
		if (FPeerSession* Session = FindSession(Incarnation))
		{
			if (Session->State == EFlecsPeerSessionState::Compatible)
			{
				const TWeakObjectPtr<APlayerController> Controller = Session->Controller;
				CloseSession(Controller, true);
			}
		}
	}
}

void UFlecsNetworkSessionSubsystem::CloseAllSessions(bool bDestroyChannels)
{
	TArray<TWeakObjectPtr<APlayerController>> Controllers;
	PeerSessions.GenerateKeyArray(Controllers);
	for (const TWeakObjectPtr<APlayerController>& Controller : Controllers)
	{
		CloseSession(Controller, bDestroyChannels);
	}
}

void UFlecsNetworkSessionSubsystem::HandleWorldAboutToDetach(UWorld& World)
{
	check(IsInGameThread());
	if (&World == GetWorld())
	{
		bIsClosing = true;
		if (WorkerBridgeState.IsValid())
		{
			WorkerBridgeState->bLifecycleRetired.store(true, std::memory_order_release);
		}
		CloseAllSessions(World.GetNetMode() != NM_Client);
	}
}

FConnectionIncarnation UFlecsNetworkSessionSubsystem::AllocateIncarnation()
{
	++NextConnectionIncarnation;
	if (NextConnectionIncarnation == 0)
	{
		++NextConnectionIncarnation;
	}
	LastIncarnation.Value = NextConnectionIncarnation;
	return LastIncarnation;
}

UFlecsNetworkSessionSubsystem::FPeerSession*
UFlecsNetworkSessionSubsystem::FindSession(FConnectionIncarnation Incarnation)
{
	for (TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (Pair.Value.Incarnation == Incarnation)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

const UFlecsNetworkSessionSubsystem::FPeerSession*
UFlecsNetworkSessionSubsystem::FindSession(FConnectionIncarnation Incarnation) const
{
	for (const TPair<TWeakObjectPtr<APlayerController>, FPeerSession>& Pair : PeerSessions)
	{
		if (Pair.Value.Incarnation == Incarnation)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}
