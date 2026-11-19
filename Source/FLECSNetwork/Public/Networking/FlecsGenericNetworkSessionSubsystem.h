#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ModRegistry/ModSchemaRegistry.h"
#include "Networking/FlecsNetworkSessionTypes.h"
#include "FlecsGenericNetworkSessionSubsystem.generated.h"

class APlayerController;
class AFlecsReplicationChannel;
struct FFlecsCommandEnvelope;
struct FFlecsReplicationRecord;
class UNetConnection;
class UWorld;
class UFlecsGameInstanceSubsystem;
struct FFlecsGenericWorkerBridgeState;
struct FFlecsGenericPeerSchedulingRuntime;

/**
 * Game-thread owner of per-connection Iris channel lifecycle. A peer remains
 * unusable until trusted handshake code confirms its manifest and server epoch.
 * This subsystem never accesses the live Flecs world.
 */
UCLASS()
class FLECSNETWORK_API UFlecsNetworkSessionSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Install before world creation; the callback runs synchronously on the game thread. */
	static bool SetDefaultConfigurator(FFlecsNetworkSessionConfigurator Configurator, uint32 ProtocolVersion);
	static void ClearDefaultConfigurator();
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override;

	void GetPeerSessions(TArray<FFlecsPeerSessionView>& OutSessions) const;
	/** Register native schemas before the registry is frozen and any peer handshake starts. */
	FlecsModRegistry::FRegistry* GetModRegistryForSetup();
	bool RegisterTrustedPermissionRule(const FString& PermissionStableId, FFlecsTrustedPermissionRule Rule,
	                                   FString& OutError);
	/** Set the optional game-thread initial-entity factory before registry freeze. */
	bool SetPeerSpawnFactory(FFlecsPeerSpawnFactory Factory, FString& OutError);
	/** Set an optional per-peer interest and budget provider before registry freeze. */
	bool SetPeerInterestProvider(FFlecsPeerInterestProvider Provider, FString& OutError);
	bool RegisterTrustedSpatialDefinition(const FString& DefinitionStableId, EFlecsTrustedSpatialPolicy Policy,
	                                      FFlecsTrustedPositionAccessor Accessor, FString& OutError);
	bool FreezeModRegistry(uint32 ProtocolVersion, FString& OutError);
	bool IsModRegistryFrozen() const { return bModRegistryFrozen; }
	bool IsPeerCompatible(FConnectionIncarnation Incarnation, uint64& OutServerWorldEpoch) const;
	bool GetPeerSessionMapping(FConnectionIncarnation Incarnation, FlecsModRegistry::FSessionMapping& OutMapping) const;
	AFlecsReplicationChannel* GetCompatibleChannel(FConnectionIncarnation Incarnation) const;

	bool RejectPeerManifest(FConnectionIncarnation Incarnation, const FString& Reason);
	/** Game-thread notifications; schedule channel recreation with RequestChannelRecreation. */
	FFlecsSessionEventDelegate OnSessionEvent;
	/** Request same-world server channel replacement on a later tick. */
	bool RequestChannelRecreation(FConnectionIncarnation Incarnation);
	/** Queue an unowned authoritative spawn; SharedSpawnFinished reports completion. */
	bool QueueSharedEntitySpawn(FFlecsPeerSpawnRequest Request, uint64& OutRequestId, FString& OutError);
	/** Queue one peer-owned spawn; PeerSpawnFinished reports worker execution, not queue admission. */
	bool QueuePeerEntitySpawn(FConnectionIncarnation Incarnation, FFlecsPeerSpawnRequest Request,
		uint64& OutRequestId, FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
	/** Opt in to inspecting one registered component on the client worker after batch apply. */
	bool SetClientInspectionComponent(const FString& StableComponentId);
#endif

private:
	struct FPeerSession
	{
		FConnectionIncarnation Incarnation;
		EFlecsPeerSessionState State = EFlecsPeerSessionState::AwaitingChannel;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<UNetConnection> Connection;
		TWeakObjectPtr<AFlecsReplicationChannel> Channel;
		uint64 LocalAttachmentEpoch = 0;
		uint64 ServerWorldEpoch = 0;
		uint64 LastQueuedSnapshotRevision = 0;
		FString RejectionReason;
		FlecsModRegistry::FSessionMapping SessionMapping;
		bool bHandshakeStarted = false;
		bool bManifestHandshakeCompleted = false;
		TSharedPtr<FFlecsGenericPeerSchedulingRuntime, ESPMode::ThreadSafe> Runtime;
	};

	bool TryConfigureSession();
	void ReconcileConnections();
	void ReconcileServerConnections(UWorld& World);
	void ReconcileClientChannels(UWorld& World);
	void StartPendingServerHandshakes();
	bool InstallWorkerIntegration(FString& OutError);
	bool InitializeClientModel(FPeerSession& Session);
	void ProcessServerReplication();
	void ProcessClientReplication();
	void ProcessServerCommandResults();
	void QueueRetirementBarriers();
	void RetireConnectionIncarnation(FConnectionIncarnation Incarnation, uint64 RetiredServerWorldEpoch);
	void QueueClientModelReset(FConnectionIncarnation Incarnation, uint64 ExpectedLocalEpoch);
	void RetryPendingClientModelReset();
	void BindPeerChannel(FPeerSession& Session);
	void HandleCompleteClientStateBatch(AFlecsReplicationChannel* Channel, uint64 SnapshotServerWorldEpoch,
	                                    uint64 SnapshotRevision, uint32 SimulationTick,
	                                    const TArray<FFlecsReplicationRecord>& Records);
	void HandleSnapshotTransferFinished(AFlecsReplicationChannel* Channel, uint64 ServerWorldEpoch,
	                                    uint64 TransferRevision, bool bSucceeded);
	void TryQueueDeltaAcknowledgement(FPeerSession& Session);
	bool AdmitDiscreteCommand(AFlecsReplicationChannel* Channel, const FFlecsCommandEnvelope& Envelope);
	void QueueContinuousCommand(AFlecsReplicationChannel* Channel, const FFlecsCommandEnvelope& Envelope);
	void ProcessPendingChannelHandshakes();
	void HandleManifestDescriptor(AFlecsReplicationChannel* Channel, uint64 InServerWorldEpoch,
	                              const TArray<uint8>& Bytes);
	void HandleManifestHandshakeFinished(AFlecsReplicationChannel* Channel, uint64 InServerWorldEpoch,
	                                     const FString& Reason);
	FPeerSession* FindSessionByChannel(AFlecsReplicationChannel* Channel);
	void CloseSession(const TWeakObjectPtr<APlayerController>& Controller, bool bDestroyChannel);
	void CloseAllSessions(bool bDestroyChannels);
	void ProcessRequestedChannelRecreations();
	void EmitPeerEvent(const FPeerSession& Session, EFlecsSessionEventKind Kind,
		uint64 Revision = 0, uint32 SimulationTick = 0, uint32 Sequence = 0,
		uint8 Result = 0, const FString& Reason = FString(),
		const FlecsSimulation::FNetworkEntityIdentity* InspectedIdentity = nullptr,
		const TArray<uint8>* InspectedBytes = nullptr);
	void HandleWorldAboutToDetach(UWorld& World);
	FConnectionIncarnation AllocateIncarnation();
	FPeerSession* FindSession(FConnectionIncarnation Incarnation);
	const FPeerSession* FindSession(FConnectionIncarnation Incarnation) const;

	UPROPERTY(Transient)
	TWeakObjectPtr<UFlecsGameInstanceSubsystem> FlecsGameInstanceSubsystem;

	TMap<TWeakObjectPtr<APlayerController>, FPeerSession> PeerSessions;
	FDelegateHandle WorldDetachHandle;
	FConnectionIncarnation LastIncarnation;
	uint64 LocalAttachmentEpoch = 0;
	uint64 ServerWorldEpoch = 0;
	uint32 SessionProtocolVersion = 0;
	uint64 NextSpawnRequestId = 0;
	TSet<uint64> PendingSharedSpawns;
	FlecsModRegistry::FRegistry ModRegistry;
	TMap<FString, FFlecsTrustedPermissionRule> TrustedPermissionRules;
	TMap<FString, FFlecsTrustedSpatialDefinition> TrustedSpatialDefinitions;
	FFlecsPeerSpawnFactory PeerSpawnFactory;
	FFlecsPeerInterestProvider PeerInterestProvider;
	TSharedPtr<FlecsModRegistry::FRegistry, ESPMode::ThreadSafe> FrozenWorkerRegistry;
	TSharedPtr<FFlecsGenericWorkerBridgeState, ESPMode::ThreadSafe> WorkerBridgeState;
	TArray<uint8> LocalManifestDescriptorBytes;
	bool bSessionConfigured = false;
	bool bModRegistryFrozen = false;
	bool bModRegistrySetupFailed = false;
	bool bRetirementFailClosed = false;
	uint64 PendingClientModelResetIncarnation = 0;
	uint64 PendingClientModelResetLocalEpoch = 0;
	TSet<uint64> PendingChannelRecreations;
#if WITH_DEV_AUTOMATION_TESTS
	FString ClientInspectionComponent;
#endif
	bool bIsClosing = false;
};
