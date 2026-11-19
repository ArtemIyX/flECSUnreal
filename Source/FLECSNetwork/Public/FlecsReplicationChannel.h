#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Iris/ReplicationState/IrisFastArraySerializer.h"
#include "FlecsBoundedPayload.h"
#include "FlecsReplicationChannel.generated.h"

class AFlecsReplicationChannel;
class APlayerController;

/** Bounded chunk used only by the pre-session manifest handshake. */
USTRUCT()
struct FLECSNETWORK_API FFlecsManifestChunk
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;
	UPROPERTY()
	uint32 TransferId = 0;
	UPROPERTY()
	uint16 ChunkIndex = 0;
	UPROPERTY()
	uint16 ChunkCount = 0;
	UPROPERTY()
	uint32 TotalBytes = 0;
	UPROPERTY()
	FFlecsBoundedPayload Payload;
};

USTRUCT()
struct FLECSNETWORK_API FFlecsCommandEnvelope
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;
	UPROPERTY()
	uint32 Sequence = 0;
	UPROPERTY()
	uint32 TargetNetworkEntityId = 0;
	UPROPERTY()
	uint16 TargetEntityGeneration = 0;
	UPROPERTY()
	uint32 RequestedSimulationTick = 0;
	UPROPERTY()
	uint16 CommandWireId = 0;
	UPROPERTY()
	FFlecsBoundedPayload Payload;
};

USTRUCT()
struct FLECSNETWORK_API FFlecsServerTickHeartbeat
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 ServerWorldEpoch = 0;

	UPROPERTY()
	uint32 SimulationTick = 0;
};

/** A bounded, versioned state record. Identity values are assigned by the server, never by local Flecs IDs. */
USTRUCT()
struct FLECSNETWORK_API FFlecsReplicationRecord : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 WorldEpoch = 0;

	UPROPERTY()
	uint32 NetworkEntityId = 0;

	UPROPERTY()
	uint16 EntityGeneration = 0;

	UPROPERTY()
	uint16 DefinitionWireId = 0;

	UPROPERTY()
	uint16 ComponentWireId = 0;

	UPROPERTY()
	uint32 SourceSimulationTick = 0;

	/** 0 = upsert, 1 = component removal, 2 = true despawn, 3 = entity create, 4 = leave relevance. */
	UPROPERTY()
	uint8 Operation = 0;

	UPROPERTY()
	uint64 SnapshotRevision = 0;

	UPROPERTY()
	uint32 SnapshotTransferId = 0;

	UPROPERTY()
	uint16 SnapshotRecordIndex = 0;

	UPROPERTY()
	uint16 SnapshotRecordCount = 0;

	UPROPERTY()
	uint32 SnapshotSimulationTick = 0;

	UPROPERTY()
	FFlecsBoundedPayload Payload;
};

USTRUCT()
struct FLECSNETWORK_API FFlecsReplicationRecordArray : public FIrisFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FFlecsReplicationRecord> Items;

	/** Set by the owning actor; replication callbacks remain on Unreal's game thread. */
	AFlecsReplicationChannel* Owner = nullptr;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FFlecsReplicationRecord, FFlecsReplicationRecordArray>(Items, DeltaParms, *this);
	}

	void PostReplicatedReceive(const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters);
};

template<>
struct TStructOpsTypeTraits<FFlecsReplicationRecordArray> : TStructOpsTypeTraitsBase2<FFlecsReplicationRecordArray>
{
	enum { WithNetDeltaSerializer = true };
};

DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsReplicationSnapshotReceived, const TArray<FFlecsReplicationRecord>&);
DECLARE_MULTICAST_DELEGATE_FourParams(FFlecsCompleteStateBatchReceived, uint64, uint64, uint32, const TArray<FFlecsReplicationRecord>&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FFlecsSnapshotTransferFinished, uint64, uint64, bool);
DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsDiscreteCommandReceived, const FFlecsCommandEnvelope&);
DECLARE_MULTICAST_DELEGATE_OneParam(FFlecsContinuousCommandReceived, const FFlecsCommandEnvelope&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FFlecsDiscreteCommandResultReceived, uint32, uint8);
DECLARE_MULTICAST_DELEGATE_TwoParams(FFlecsServerTickHeartbeatReceived, uint64, uint32);

/** Result codes are transport admission outcomes; AcceptedForWorker does not claim command execution. */
namespace FlecsNetworkCommandResult
{
	constexpr uint8 AcceptedForWorker = 0;
	constexpr uint8 RejectedInvalid = 1;
	constexpr uint8 RejectedSequence = 2;
	constexpr uint8 RejectedTickWindow = 3;
	constexpr uint8 RejectedRateLimit = 4;
	constexpr uint8 RejectedWorkerUnavailable = 5;
	constexpr uint8 ExecutionAccepted = 6;
	constexpr uint8 ExecutionRejectedUnauthorized = 7;
	constexpr uint8 ExecutionRejectedStale = 8;
	constexpr uint8 ExecutionRejectedUnknownSchema = 9;
	constexpr uint8 ExecutionFailed = 10;
	constexpr uint8 ExecutionResultExpired = 11;
}

using FFlecsDiscreteCommandAdmission = TFunction<bool(const FFlecsCommandEnvelope&)>;

struct FFlecsManifestAssembly
{
	uint64 WorldEpoch = 0;
	uint32 TransferId = 0;
	uint32 TotalBytes = 0;
	uint16 ChunkCount = 0;
	uint32 ReceivedBytes = 0;
	TArray<TArray<uint8>> Chunks;
	TBitArray<> ReceivedChunks;
};

/** Per-connection owner-only Iris host. All methods, callbacks, and delegates are game-thread-only. */
UCLASS(NotBlueprintable, NotPlaceable)
class FLECSNETWORK_API AFlecsReplicationChannel : public AActor
{
	GENERATED_BODY()

public:
	AFlecsReplicationChannel();

	/** Call on the server after spawn. The owner connection determines this channel's relevance. */
	void SetChannelOwner(APlayerController* InOwner);
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Begin the server-to-client manifest exchange. Payload is produced by FManifestDescriptor::Serialize. */
	bool StartManifestHandshake(uint64 ServerWorldEpoch, const TArray<uint8>& LocalDescriptor);
	/** Client only: report its local comparison and send the bounded local descriptor to the server in ACK-paced windows. */
	bool SubmitLocalManifestDescriptor(uint64 ServerWorldEpoch, const TArray<uint8>& LocalDescriptor);
	/** Server only: complete the independent comparison of the client's descriptor. */
	bool CompleteServerManifestHandshake(uint64 ServerWorldEpoch, bool bCompatible, const FString& Reason);
	/** Reject and close a handshake after malformed input, mismatch, or timeout. */
	void RejectManifestHandshake(uint64 ServerWorldEpoch, const FString& Reason);
	bool IsManifestHandshakePending(uint64 ServerWorldEpoch) const;
	bool ConsumePendingManifestDescriptor(uint64& OutServerWorldEpoch, TArray<uint8>& OutDescriptor);
	bool ConsumePendingManifestResult(uint64& OutServerWorldEpoch, bool& bOutCompatible, FString& OutReason);

	/** Start one ACK-paced selected-state update batch. Limits are 16,384 records and 4 MiB encoded budget; overflow fails closed. */
	bool PublishUpdateBatch(uint64 InWorldEpoch, const TArray<FFlecsReplicationRecord>& Records, uint32 InSimulationTick);
	/** Game-thread diagnostic for the most recent rejected PublishUpdateBatch call, bounded to 256 characters. */
	FString GetLastUpdateBatchRejectionReason() const;
	/** True while this server channel is waiting for an owner ACK; callers should pause new selections. */
	bool IsUpdateBatchTransferActive() const;
	/** Client only: report atomic worker-application success for the assembled batch and release its final ACK. */
	bool CompleteReceivedSnapshotBatch(uint64 InWorldEpoch, uint64 InSnapshotRevision, bool bAccepted);
	/** Server only: deliver a final worker execution result for a previously admitted discrete command. */
	bool ReportDiscreteCommandExecutionResult(uint64 InWorldEpoch, uint32 Sequence, uint8 ResultCode);
	/** Server only: publish an idle-capable worker tick heartbeat without an entity update batch. */
	bool PublishServerTickHeartbeat(uint64 InWorldEpoch, uint32 InSimulationTick);
	/** Latest observed server clock, independent of the last applied state batch. */
	uint32 GetServerSimulationTick() const { return CurrentSimulationTick; }
	/** Dispatch state that arrived before the client session bridge finished initialization. */
	void ResumeReceivedState();

	/** Deprecated compatibility delegate; it is no longer broadcast because FastArray receive callbacks can expose fragments. */
	FFlecsReplicationSnapshotReceived OnSnapshotReceived;
	/** Client game-thread callback: (server epoch, batch revision, simulation tick, copied atomic delta records). */
	/** The final ACK waits for worker application. Copy records before asynchronous queueing. */
	FFlecsCompleteStateBatchReceived OnCompleteStateBatchReceived;
	/** Server game-thread callback after the final client worker-application ACK or transfer failure/timeout. */
	FFlecsSnapshotTransferFinished OnSnapshotTransferFinished;
	/** Client callback for the latest monotonic server simulation tick heartbeat. */
	FFlecsServerTickHeartbeatReceived OnServerTickHeartbeatReceived;
	/** Transport callbacks do not grant command authorization; authoritative handlers must revalidate requests on the worker. */
	FFlecsDiscreteCommandReceived OnDiscreteCommandReceived;
	FFlecsContinuousCommandReceived OnContinuousCommandReceived;
	/** Client receives validation/queue-admission results; acceptance means queued for worker validation, not executed. */
	FFlecsDiscreteCommandResultReceived OnDiscreteCommandResultReceived;
	/** Server only. Must atomically queue the command and return true only when worker admission succeeds. */
	FFlecsDiscreteCommandAdmission OnDiscreteCommandAdmission;
	/** Reliable RPC. Preserve/retry the same sequence when the returned result is a rejection. */
	void SubmitDiscreteCommand(const FFlecsCommandEnvelope& Envelope);
	void SubmitContinuousCommand(const FFlecsCommandEnvelope& Envelope);

protected:
	UPROPERTY(Replicated)
	FFlecsReplicationRecordArray ReplicatedState;

	UPROPERTY(ReplicatedUsing=OnRep_WorldEpoch)
	uint64 ReplicatedWorldEpoch = 0;

	UPROPERTY(ReplicatedUsing=OnRep_ServerTickHeartbeat)
	FFlecsServerTickHeartbeat ReplicatedServerTickHeartbeat;

	UFUNCTION()
	void OnRep_WorldEpoch();

	UFUNCTION()
	void OnRep_ServerTickHeartbeat();

private:
	friend struct FFlecsReplicationRecordArray;
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FFlecsReplicationChannelTestAccessor;
#endif
	void HandleReplicatedSnapshot();
	void HandleCommand(const FFlecsCommandEnvelope& Envelope, bool bIsDiscrete);
	void SendDiscreteCommandResult(uint64 WorldEpoch, uint32 Sequence, uint8 ResultCode);
	void PumpSnapshotWindow();
	void HandleSnapshotWindowAcknowledgement(uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision,
		uint16 HighestRecordIndex, bool bAccepted);
	void SendSnapshotWindowAcknowledgement(uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision,
		uint16 HighestRecordIndex, bool bAccepted);
	void FailSnapshotTransfer(uint64 ServerWorldEpoch, const FString& Reason);
	void SendManifestChunks(uint64 ServerWorldEpoch, const TArray<uint8>& Bytes, bool bToClient);
	void PumpManifestChunkWindow();
	void HandleManifestChunkAcknowledgement(uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted);
	void SendManifestChunkAcknowledgement(const FFlecsManifestChunk& Chunk, bool bToServer);
	bool ReceiveManifestChunk(const FFlecsManifestChunk& Chunk, TArray<uint8>& OutCompleteDescriptor);
	void HandleManifestTimeout();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	bool IsRecordBoundedAndValid(const FFlecsReplicationRecord& Record, uint64 ExpectedEpoch) const;
	UFUNCTION(Server, Unreliable)
	void ServerSubmitContinuousCommand(const FFlecsCommandEnvelope& Envelope);
	UFUNCTION(Server, Reliable)
	void ServerSubmitDiscreteCommand(const FFlecsCommandEnvelope& Envelope);
	UFUNCTION(Client, Reliable)
	void ClientReceiveManifestChunk(const FFlecsManifestChunk& Chunk);
	UFUNCTION(Server, Reliable)
	void ServerSubmitManifestChunk(const FFlecsManifestChunk& Chunk);
	UFUNCTION(Server, Reliable)
	void ServerRejectManifest(uint64 ServerWorldEpoch, uint8 ReasonCode);
	UFUNCTION(Client, Reliable)
	void ClientManifestResult(uint64 ServerWorldEpoch, bool bCompatible, const FString& Reason);
	UFUNCTION(Client, Reliable)
	void ClientDiscreteCommandResult(uint64 WorldEpoch, uint32 Sequence, uint8 ResultCode);
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeManifestChunkWindow(uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted);
	UFUNCTION(Client, Reliable)
	void ClientAcknowledgeManifestChunkWindow(uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted);
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeSnapshotWindow(uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision,
		uint16 HighestContiguousRecordIndex, bool bAccepted);

	uint64 CurrentWorldEpoch = 0;
	uint32 CurrentSimulationTick = 0;
	uint32 LastPublishedStateSimulationTick = 0;
	uint32 LastReceivedStateSimulationTick = 0;
	bool bClientStateDispatchReady = false;
	FString LastUpdateBatchRejectionReason;
	uint64 CurrentPublishedSnapshotRevision = 0;
	uint64 LastReceivedSnapshotEpoch = 0;
	uint64 LastReceivedSnapshotRevision = 0;
	uint32 NextSnapshotTransferId = 0;
	uint32 NextManifestTransferId = 0;
	uint64 HandshakeServerWorldEpoch = 0;
	uint32 ServerManifestTransferId = 0;
	struct FOutgoingManifest
	{
		bool bActive = false;
		bool bToClient = false;
		bool bWaitingForAcknowledgement = false;
		uint64 WorldEpoch = 0;
		uint32 TransferId = 0;
		uint16 ChunkCount = 0;
		uint16 NextChunkIndex = 0;
		uint16 AwaitingThroughChunk = 0;
		TArray<uint8> Bytes;
	};
	FOutgoingManifest OutgoingManifest;
	struct FOutgoingSnapshotTransfer
	{
		bool bActive = false;
		bool bWaitingForAcknowledgement = false;
		uint64 WorldEpoch = 0;
		uint32 TransferId = 0;
		uint64 Revision = 0;
		uint32 SimulationTick = 0;
		uint16 RecordCount = 0;
		uint16 NextRecordIndex = 0;
		uint16 AwaitingThroughRecordIndex = 0;
		double LastProgressSeconds = 0.0;
		TArray<FFlecsReplicationRecord> Records;
	};
	FOutgoingSnapshotTransfer OutgoingSnapshotTransfer;
	struct FIncomingSnapshotTransfer
	{
		bool bActive = false;
		bool bWaitingForWorker = false;
		uint64 WorldEpoch = 0;
		uint32 TransferId = 0;
		uint64 Revision = 0;
		uint32 SimulationTick = 0;
		uint16 RecordCount = 0;
		uint16 NextAcknowledgementStartIndex = 0;
		uint32 ReceivedRecordCount = 0;
		uint64 ReceivedPayloadBytes = 0;
		uint64 ReceivedWireFramingBytes = 0;
		double LastProgressSeconds = 0.0;
		TArray<FFlecsReplicationRecord> Records;
		TBitArray<> ReceivedIndices;
	};
	FIncomingSnapshotTransfer IncomingSnapshotTransfer;
	double ManifestHandshakeStartSeconds = 0.0;
	bool bManifestHandshakePending = false;
	bool bLocalManifestCompared = false;
	bool bClientManifestValidated = false;
	bool bServerManifestValidated = false;
	FFlecsManifestAssembly IncomingManifest;
	uint64 PendingDescriptorEpoch = 0;
	TArray<uint8> PendingManifestDescriptor;
	uint64 PendingResultEpoch = 0;
	bool bPendingManifestResult = false;
	bool bPendingManifestResultCompatible = false;
	FString PendingManifestResultReason;
	uint32 LastDiscreteCommandSequence = 0;
	TMap<uint32, uint8> PendingDiscreteCommandResults;
	TMap<uint32, uint8> CompletedDiscreteCommandResults;
	TArray<uint32> CompletedDiscreteCommandSequenceOrder;
	uint32 LastContinuousCommandSequence = 0;
	double DiscreteCommandWindowStartSeconds = 0.0;
	double ContinuousCommandWindowStartSeconds = 0.0;
	uint16 DiscreteCommandsInWindow = 0;
	uint16 ContinuousCommandsInWindow = 0;
	TMap<uint32, uint16> HighestPublishedGenerations;
	TMap<uint32, uint16> HighestReceivedGenerations;
	TMap<uint32, uint16> HighestPublishedDefinitions;
	TMap<uint32, uint16> HighestReceivedDefinitions;
};
