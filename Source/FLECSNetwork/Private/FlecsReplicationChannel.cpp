#include "FlecsReplicationChannel.h"
#include "Diagnostics/FlecsNetworkDiagnosticsSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

namespace
{
constexpr int32 MaxRecordsPerSnapshot = 16384;
constexpr int32 MaxPayloadBytesPerRecord = 1024;
constexpr int32 MaxSnapshotPayloadBytes = 4 * 1024 * 1024;
constexpr uint32 RecordWireFramingBytes = 45;
constexpr uint16 SnapshotWindowRecords = 16;
constexpr double SnapshotTransferTimeoutSeconds = 10.0;
constexpr int32 MaxCommandPayloadBytes = 1024;
constexpr uint16 MaxDiscreteCommandsPerSecond = 20;
constexpr uint16 MaxContinuousCommandsPerSecond = 60;
constexpr int32 MaxPendingDiscreteCommandResults = 32;
constexpr int32 MaxCachedDiscreteCommandResults = 64;
constexpr uint32 MaxPastCommandTickWindow = 120;
constexpr uint32 MaxFutureCommandTickWindow = 30;
constexpr uint32 MaxManifestDescriptorBytes = 1024 * 1024;
constexpr uint32 ManifestChunkBytes = 1024;
constexpr uint16 ManifestWindowChunks = 16;
constexpr uint16 MaxManifestChunks = (MaxManifestDescriptorBytes + ManifestChunkBytes - 1) / ManifestChunkBytes;
constexpr double ManifestHandshakeTimeoutSeconds = 10.0;

struct FRecordKey
{
	uint64 Epoch = 0;
	uint32 EntityId = 0;
	uint16 Generation = 0;
	uint16 ComponentId = 0;

	bool operator==(const FRecordKey& Other) const
	{
		return Epoch == Other.Epoch && EntityId == Other.EntityId && Generation == Other.Generation
			&& ComponentId == Other.ComponentId;
	}

	friend uint32 GetTypeHash(const FRecordKey& Key)
	{
		return HashCombine(HashCombine(GetTypeHash(Key.Epoch), GetTypeHash(Key.EntityId)),
			HashCombine(GetTypeHash(Key.Generation), GetTypeHash(Key.ComponentId)));
	}
};

FRecordKey MakeKey(const FFlecsReplicationRecord& Record)
{
	return {Record.WorldEpoch, Record.NetworkEntityId, Record.EntityGeneration,
		(Record.Operation == 2 || Record.Operation == 3 || Record.Operation == 4) ? static_cast<uint16>(0)
																				  : Record.ComponentWireId};
}

bool HasSameSnapshotRecord(const FFlecsReplicationRecord& A, const FFlecsReplicationRecord& B)
{
	return A.WorldEpoch == B.WorldEpoch && A.NetworkEntityId == B.NetworkEntityId
		&& A.EntityGeneration == B.EntityGeneration && A.DefinitionWireId == B.DefinitionWireId
		&& A.ComponentWireId == B.ComponentWireId && A.SourceSimulationTick == B.SourceSimulationTick
		&& A.Operation == B.Operation && A.SnapshotRevision == B.SnapshotRevision
		&& A.SnapshotTransferId == B.SnapshotTransferId && A.SnapshotRecordIndex == B.SnapshotRecordIndex
		&& A.SnapshotRecordCount == B.SnapshotRecordCount && A.SnapshotSimulationTick == B.SnapshotSimulationTick
		&& A.Payload == B.Payload;
}

bool IsGenerationNewer(uint16 Candidate, uint16 Previous)
{
	// The worker retires an ID permanently at MAX_uint16; generations do
	// not wrap. A peer can legitimately miss more than half the range.
	return Candidate > Previous;
}

void CopyState(FFlecsReplicationRecord& Destination, const FFlecsReplicationRecord& Source)
{
	Destination.WorldEpoch = Source.WorldEpoch;
	Destination.NetworkEntityId = Source.NetworkEntityId;
	Destination.EntityGeneration = Source.EntityGeneration;
	Destination.DefinitionWireId = Source.DefinitionWireId;
	Destination.ComponentWireId = Source.ComponentWireId;
	Destination.SourceSimulationTick = Source.SourceSimulationTick;
	Destination.Operation = Source.Operation;
	Destination.SnapshotRevision = Source.SnapshotRevision;
	Destination.SnapshotTransferId = Source.SnapshotTransferId;
	Destination.SnapshotRecordIndex = Source.SnapshotRecordIndex;
	Destination.SnapshotRecordCount = Source.SnapshotRecordCount;
	Destination.SnapshotSimulationTick = Source.SnapshotSimulationTick;
	Destination.Payload = Source.Payload;
}
}

void FFlecsReplicationRecordArray::PostReplicatedReceive(
	const FFastArraySerializer::FPostReplicatedReceiveParameters& Parameters)
{
	(void)Parameters;
	if (Owner)
	{
		Owner->HandleReplicatedSnapshot();
	}
}

AFlecsReplicationChannel::AFlecsReplicationChannel()
{
	ReplicatedState.Owner = this;
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	bAlwaysRelevant = false;
	SetReplicateMovement(false);
	bNetUseOwnerRelevancy = false;
	bNetLoadOnClient = false;
	SetNetUpdateFrequency(30.0f);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AFlecsReplicationChannel::BeginPlay()
{
	Super::BeginPlay();
	ReplicatedState.Owner = this;
}

void AFlecsReplicationChannel::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double NowSeconds = FPlatformTime::Seconds();
	if (bManifestHandshakePending && NowSeconds - ManifestHandshakeStartSeconds > ManifestHandshakeTimeoutSeconds)
	{
		HandleManifestTimeout();
	}
	if (OutgoingSnapshotTransfer.bActive
		&& NowSeconds - OutgoingSnapshotTransfer.LastProgressSeconds > SnapshotTransferTimeoutSeconds)
	{
		FailSnapshotTransfer(OutgoingSnapshotTransfer.WorldEpoch,
			TEXT("Snapshot transfer timed out waiting for an Iris window acknowledgement."));
	}
	if (IncomingSnapshotTransfer.bActive
		&& NowSeconds - IncomingSnapshotTransfer.LastProgressSeconds > SnapshotTransferTimeoutSeconds)
	{
		FailSnapshotTransfer(IncomingSnapshotTransfer.WorldEpoch,
			TEXT("Snapshot transfer timed out waiting for the worker or next window."));
	}
}

bool AFlecsReplicationChannel::StartManifestHandshake(uint64 ServerWorldEpoch, const TArray<uint8>& LocalDescriptor)
{
	check(IsInGameThread());
	if (!HasAuthority() || bServerManifestValidated || bManifestHandshakePending || ServerWorldEpoch == 0
		|| LocalDescriptor.IsEmpty() || LocalDescriptor.Num() > MaxManifestDescriptorBytes || !IsValid(GetOwner())
		|| OutgoingSnapshotTransfer.bActive)
	{
		return false;
	}
	if (CurrentWorldEpoch != ServerWorldEpoch)
	{
		CurrentSimulationTick = 0;
		LastPublishedStateSimulationTick = 0;
		LastReceivedStateSimulationTick = 0;
		CurrentPublishedSnapshotRevision = 0;
		LastDiscreteCommandSequence = 0;
		LastContinuousCommandSequence = 0;
		PendingDiscreteCommandResults.Empty();
		CompletedDiscreteCommandResults.Empty();
		CompletedDiscreteCommandSequenceOrder.Empty();
		DiscreteCommandWindowStartSeconds = 0.0;
		ContinuousCommandWindowStartSeconds = 0.0;
		DiscreteCommandsInWindow = 0;
		ContinuousCommandsInWindow = 0;
		HighestPublishedGenerations.Empty();
		HighestPublishedDefinitions.Empty();
		if (!ReplicatedState.Items.IsEmpty())
		{
			ReplicatedState.Items.Reset();
			ReplicatedState.MarkArrayDirty();
		}
	}
	HandshakeServerWorldEpoch = ServerWorldEpoch;
	CurrentWorldEpoch = ServerWorldEpoch;
	bManifestHandshakePending = true;
	bLocalManifestCompared = false;
	bClientManifestValidated = false;
	bServerManifestValidated = false;
	IncomingManifest = FFlecsManifestAssembly();
	OutgoingManifest = FOutgoingManifest();
	PendingDescriptorEpoch = 0;
	PendingManifestDescriptor.Reset();
	bPendingManifestResult = false;
	PendingResultEpoch = 0;
	ManifestHandshakeStartSeconds = FPlatformTime::Seconds();
	SendManifestChunks(ServerWorldEpoch, LocalDescriptor, true);
	return true;
}

bool AFlecsReplicationChannel::SubmitLocalManifestDescriptor(
	uint64 ServerWorldEpoch, const TArray<uint8>& LocalDescriptor)
{
	check(IsInGameThread());
	if (HasAuthority() || !bManifestHandshakePending || bLocalManifestCompared || ServerWorldEpoch == 0
		|| ServerWorldEpoch != HandshakeServerWorldEpoch || LocalDescriptor.IsEmpty()
		|| LocalDescriptor.Num() > MaxManifestDescriptorBytes)
	{
		return false;
	}
	bLocalManifestCompared = true;
	SendManifestChunks(ServerWorldEpoch, LocalDescriptor, false);
	return true;
}

bool AFlecsReplicationChannel::CompleteServerManifestHandshake(
	uint64 ServerWorldEpoch, bool bCompatible, const FString& Reason)
{
	check(IsInGameThread());
	if (!HasAuthority() || !bManifestHandshakePending || ServerWorldEpoch == 0
		|| ServerWorldEpoch != HandshakeServerWorldEpoch)
	{
		return false;
	}
	if (!bCompatible)
	{
		RejectManifestHandshake(ServerWorldEpoch, Reason);
		return true;
	}
	bServerManifestValidated = true;
	bManifestHandshakePending = false;
	bPendingManifestResult = true;
	PendingResultEpoch = ServerWorldEpoch;
	bPendingManifestResultCompatible = true;
	PendingManifestResultReason.Reset();
	ClientManifestResult(ServerWorldEpoch, true, FString());
	return true;
}

void AFlecsReplicationChannel::RejectManifestHandshake(uint64 ServerWorldEpoch, const FString& Reason)
{
	check(IsInGameThread());
	if (ServerWorldEpoch == 0 || ServerWorldEpoch != HandshakeServerWorldEpoch)
	{
		return;
	}
	bManifestHandshakePending = false;
	bClientManifestValidated = false;
	bServerManifestValidated = false;
	IncomingManifest = FFlecsManifestAssembly();
	OutgoingManifest = FOutgoingManifest();
	PendingDescriptorEpoch = 0;
	PendingManifestDescriptor.Reset();
	const FString BoundedReason = Reason.IsEmpty() ? FString(TEXT("The peer session was rejected.")) : Reason.Left(512);
	bPendingManifestResult = true;
	PendingResultEpoch = ServerWorldEpoch;
	bPendingManifestResultCompatible = false;
	PendingManifestResultReason = BoundedReason;
	if (HasAuthority())
	{
		ClientManifestResult(ServerWorldEpoch, false, BoundedReason);
	}
	else
	{
		ServerRejectManifest(ServerWorldEpoch, 1);
	}
}

bool AFlecsReplicationChannel::IsManifestHandshakePending(uint64 ServerWorldEpoch) const
{
	return bManifestHandshakePending && HandshakeServerWorldEpoch == ServerWorldEpoch;
}

bool AFlecsReplicationChannel::ConsumePendingManifestDescriptor(
	uint64& OutServerWorldEpoch, TArray<uint8>& OutDescriptor)
{
	check(IsInGameThread());
	if (PendingDescriptorEpoch == 0 || PendingManifestDescriptor.IsEmpty())
	{
		return false;
	}
	OutServerWorldEpoch = PendingDescriptorEpoch;
	OutDescriptor = MoveTemp(PendingManifestDescriptor);
	PendingDescriptorEpoch = 0;
	return true;
}

bool AFlecsReplicationChannel::ConsumePendingManifestResult(
	uint64& OutServerWorldEpoch, bool& bOutCompatible, FString& OutReason)
{
	check(IsInGameThread());
	if (!bPendingManifestResult)
	{
		return false;
	}
	OutServerWorldEpoch = PendingResultEpoch;
	bOutCompatible = bPendingManifestResultCompatible;
	OutReason = MoveTemp(PendingManifestResultReason);
	bPendingManifestResult = false;
	PendingResultEpoch = 0;
	return true;
}

void AFlecsReplicationChannel::SendManifestChunks(uint64 ServerWorldEpoch, const TArray<uint8>& Bytes, bool bToClient)
{
	check(IsInGameThread());
	if (Bytes.IsEmpty() || Bytes.Num() > MaxManifestDescriptorBytes || OutgoingManifest.bActive)
	{
		RejectManifestHandshake(
			ServerWorldEpoch, TEXT("Manifest transfer is invalid or another transfer is still active."));
		return;
	}
	uint32 TransferId = ServerManifestTransferId;
	if (bToClient)
	{
		++NextManifestTransferId;
		if (NextManifestTransferId == 0)
		{
			++NextManifestTransferId;
		}
		TransferId = NextManifestTransferId;
		ServerManifestTransferId = TransferId;
	}
	else if (TransferId == 0)
	{
		RejectManifestHandshake(ServerWorldEpoch, TEXT("Manifest response has no active server challenge."));
		return;
	}
	OutgoingManifest = FOutgoingManifest();
	OutgoingManifest.bActive = true;
	OutgoingManifest.bToClient = bToClient;
	OutgoingManifest.WorldEpoch = ServerWorldEpoch;
	OutgoingManifest.TransferId = TransferId;
	OutgoingManifest.ChunkCount = static_cast<uint16>((Bytes.Num() + ManifestChunkBytes - 1) / ManifestChunkBytes);
	OutgoingManifest.Bytes = Bytes;
	if (UWorld* World = GetWorld())
		if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
			Diagnostics->RecordPayload(2, true, Bytes.Num());
	PumpManifestChunkWindow();
}

void AFlecsReplicationChannel::PumpManifestChunkWindow()
{
	check(IsInGameThread());
	if (!OutgoingManifest.bActive || OutgoingManifest.bWaitingForAcknowledgement)
	{
		return;
	}
	const uint16 WindowEnd = FMath::Min<uint16>(
		OutgoingManifest.ChunkCount, static_cast<uint16>(OutgoingManifest.NextChunkIndex + ManifestWindowChunks));
	OutgoingManifest.AwaitingThroughChunk = WindowEnd - 1;
	OutgoingManifest.bWaitingForAcknowledgement = true;
	for (uint16 ChunkIndex = OutgoingManifest.NextChunkIndex; ChunkIndex < WindowEnd; ++ChunkIndex)
	{
		const int32 Offset = static_cast<int32>(ChunkIndex) * ManifestChunkBytes;
		const int32 ChunkSize = FMath::Min<int32>(ManifestChunkBytes, OutgoingManifest.Bytes.Num() - Offset);
		FFlecsManifestChunk Chunk;
		Chunk.WorldEpoch = OutgoingManifest.WorldEpoch;
		Chunk.TransferId = OutgoingManifest.TransferId;
		Chunk.ChunkIndex = ChunkIndex;
		Chunk.ChunkCount = OutgoingManifest.ChunkCount;
		Chunk.TotalBytes = static_cast<uint32>(OutgoingManifest.Bytes.Num());
		Chunk.Payload.Append(OutgoingManifest.Bytes.GetData() + Offset, ChunkSize);
		if (OutgoingManifest.bToClient)
		{
			ClientReceiveManifestChunk(Chunk);
		}
		else
		{
			ServerSubmitManifestChunk(Chunk);
		}
	}
	OutgoingManifest.NextChunkIndex = WindowEnd;
}

void AFlecsReplicationChannel::HandleManifestChunkAcknowledgement(
	uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted)
{
	check(IsInGameThread());
	if (!OutgoingManifest.bActive || !OutgoingManifest.bWaitingForAcknowledgement
		|| ServerWorldEpoch != OutgoingManifest.WorldEpoch || TransferId != OutgoingManifest.TransferId
		|| HighestContiguousChunk != OutgoingManifest.AwaitingThroughChunk)
	{
		return;
	}
	if (!bAccepted)
	{
		RejectManifestHandshake(ServerWorldEpoch, TEXT("Remote rejected a manifest transfer window."));
		return;
	}
	OutgoingManifest.bWaitingForAcknowledgement = false;
	ManifestHandshakeStartSeconds = FPlatformTime::Seconds();
	if (OutgoingManifest.NextChunkIndex >= OutgoingManifest.ChunkCount)
	{
		OutgoingManifest = FOutgoingManifest();
		return;
	}
	PumpManifestChunkWindow();
}

void AFlecsReplicationChannel::SendManifestChunkAcknowledgement(const FFlecsManifestChunk& Chunk, bool bToServer)
{
	const bool bWindowEnd =
		((Chunk.ChunkIndex + 1) % ManifestWindowChunks) == 0 || Chunk.ChunkIndex + 1 == Chunk.ChunkCount;
	if (!bWindowEnd)
	{
		return;
	}
	if (bToServer)
	{
		ServerAcknowledgeManifestChunkWindow(Chunk.WorldEpoch, Chunk.TransferId, Chunk.ChunkIndex, true);
	}
	else
	{
		ClientAcknowledgeManifestChunkWindow(Chunk.WorldEpoch, Chunk.TransferId, Chunk.ChunkIndex, true);
	}
}

void AFlecsReplicationChannel::ServerAcknowledgeManifestChunkWindow_Implementation(
	uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted)
{
	check(IsInGameThread());
	const APlayerController* OwningController = Cast<APlayerController>(GetOwner());
	if (HasAuthority() && OwningController && OwningController->GetNetConnection() && OutgoingManifest.bToClient)
	{
		HandleManifestChunkAcknowledgement(ServerWorldEpoch, TransferId, HighestContiguousChunk, bAccepted);
	}
}

void AFlecsReplicationChannel::ClientAcknowledgeManifestChunkWindow_Implementation(
	uint64 ServerWorldEpoch, uint32 TransferId, uint16 HighestContiguousChunk, bool bAccepted)
{
	check(IsInGameThread());
	if (!HasAuthority() && OutgoingManifest.bActive && !OutgoingManifest.bToClient)
	{
		HandleManifestChunkAcknowledgement(ServerWorldEpoch, TransferId, HighestContiguousChunk, bAccepted);
	}
}

bool AFlecsReplicationChannel::ReceiveManifestChunk(
	const FFlecsManifestChunk& Chunk, TArray<uint8>& OutCompleteDescriptor)
{
	check(IsInGameThread());
	OutCompleteDescriptor.Reset();
	if (!bManifestHandshakePending || PendingDescriptorEpoch != 0 || Chunk.WorldEpoch != HandshakeServerWorldEpoch
		|| Chunk.TotalBytes == 0 || Chunk.TotalBytes > MaxManifestDescriptorBytes || !Chunk.Payload.IsValid()
		|| Chunk.Payload.IsEmpty() || Chunk.Payload.Num() > ManifestChunkBytes || Chunk.ChunkCount == 0
		|| Chunk.ChunkCount > MaxManifestChunks || Chunk.ChunkIndex >= Chunk.ChunkCount
		|| Chunk.ChunkCount != (Chunk.TotalBytes + ManifestChunkBytes - 1) / ManifestChunkBytes)
	{
		return false;
	}
	if (IncomingManifest.TransferId == 0)
	{
		if (Chunk.ChunkIndex != 0)
		{
			return false;
		}
		IncomingManifest.WorldEpoch = Chunk.WorldEpoch;
		IncomingManifest.TransferId = Chunk.TransferId;
		IncomingManifest.TotalBytes = Chunk.TotalBytes;
		IncomingManifest.ChunkCount = Chunk.ChunkCount;
		IncomingManifest.Chunks.SetNum(Chunk.ChunkCount);
		IncomingManifest.ReceivedChunks.Init(false, Chunk.ChunkCount);
	}
	const uint32 ExpectedIndex = IncomingManifest.ReceivedBytes / ManifestChunkBytes;
	if (IncomingManifest.WorldEpoch != Chunk.WorldEpoch || IncomingManifest.TransferId != Chunk.TransferId
		|| IncomingManifest.TotalBytes != Chunk.TotalBytes || IncomingManifest.ChunkCount != Chunk.ChunkCount
		|| Chunk.ChunkIndex != ExpectedIndex || IncomingManifest.ReceivedChunks[Chunk.ChunkIndex]
		|| Chunk.Payload.Num()
			   != FMath::Min<uint32>(ManifestChunkBytes, Chunk.TotalBytes - Chunk.ChunkIndex * ManifestChunkBytes)
		|| static_cast<uint32>(Chunk.Payload.Num()) > IncomingManifest.TotalBytes - IncomingManifest.ReceivedBytes)
	{
		IncomingManifest = FFlecsManifestAssembly();
		return false;
	}
	IncomingManifest.Chunks[Chunk.ChunkIndex] = Chunk.Payload;
	IncomingManifest.ReceivedChunks[Chunk.ChunkIndex] = true;
	IncomingManifest.ReceivedBytes += static_cast<uint32>(Chunk.Payload.Num());
	ManifestHandshakeStartSeconds = FPlatformTime::Seconds();
	if (IncomingManifest.ReceivedBytes != IncomingManifest.TotalBytes)
	{
		return true;
	}
	OutCompleteDescriptor.Reserve(IncomingManifest.TotalBytes);
	for (const TArray<uint8>& Part : IncomingManifest.Chunks)
	{
		OutCompleteDescriptor.Append(Part);
	}
	const bool bComplete = OutCompleteDescriptor.Num() == IncomingManifest.TotalBytes;
	if (bComplete)
		if (UWorld* World = GetWorld())
			if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
				Diagnostics->RecordPayload(2, false, OutCompleteDescriptor.Num());
	IncomingManifest = FFlecsManifestAssembly();
	return bComplete;
}

void AFlecsReplicationChannel::ClientReceiveManifestChunk_Implementation(const FFlecsManifestChunk& Chunk)
{
	check(IsInGameThread());
	if (HasAuthority() || bClientManifestValidated)
	{
		return;
	}
	if (!bManifestHandshakePending)
	{
		bClientStateDispatchReady = false;
		if (CurrentWorldEpoch != Chunk.WorldEpoch)
		{
			HighestReceivedGenerations.Empty();
			HighestReceivedDefinitions.Empty();
			LastReceivedSnapshotEpoch = 0;
			LastReceivedSnapshotRevision = 0;
			CurrentSimulationTick = 0;
			LastPublishedStateSimulationTick = 0;
			LastReceivedStateSimulationTick = 0;
			IncomingSnapshotTransfer = FIncomingSnapshotTransfer();
		}
		HandshakeServerWorldEpoch = Chunk.WorldEpoch;
		CurrentWorldEpoch = Chunk.WorldEpoch;
		bManifestHandshakePending = Chunk.WorldEpoch != 0;
		ManifestHandshakeStartSeconds = FPlatformTime::Seconds();
	}
	if (IncomingManifest.TransferId == 0 && Chunk.ChunkIndex == 0)
	{
		ServerManifestTransferId = Chunk.TransferId;
	}
	TArray<uint8> CompleteDescriptor;
	const bool bValid = ReceiveManifestChunk(Chunk, CompleteDescriptor);
	if (!bValid)
	{
		RejectManifestHandshake(
			HandshakeServerWorldEpoch, TEXT("Malformed, duplicate, out-of-order, or stale manifest chunk."));
		return;
	}
	if (!CompleteDescriptor.IsEmpty())
	{
		PendingDescriptorEpoch = Chunk.WorldEpoch;
		PendingManifestDescriptor = CompleteDescriptor;
	}
	SendManifestChunkAcknowledgement(Chunk, true);
}

void AFlecsReplicationChannel::ServerSubmitManifestChunk_Implementation(const FFlecsManifestChunk& Chunk)
{
	check(IsInGameThread());
	const APlayerController* OwningController = Cast<APlayerController>(GetOwner());
	if (!HasAuthority() || !OwningController || !OwningController->GetNetConnection() || !bManifestHandshakePending
		|| bServerManifestValidated || Chunk.TransferId != ServerManifestTransferId)
	{
		return;
	}
	TArray<uint8> CompleteDescriptor;
	const bool bValid = ReceiveManifestChunk(Chunk, CompleteDescriptor);
	if (!bValid)
	{
		RejectManifestHandshake(
			HandshakeServerWorldEpoch, TEXT("Malformed, duplicate, out-of-order, or stale manifest chunk."));
		return;
	}
	if (!CompleteDescriptor.IsEmpty())
	{
		PendingDescriptorEpoch = Chunk.WorldEpoch;
		PendingManifestDescriptor = CompleteDescriptor;
	}
	SendManifestChunkAcknowledgement(Chunk, false);
}

void AFlecsReplicationChannel::ServerRejectManifest_Implementation(uint64 ServerWorldEpoch, uint8 ReasonCode)
{
	check(IsInGameThread());
	const APlayerController* OwningController = Cast<APlayerController>(GetOwner());
	if (HasAuthority() && OwningController && OwningController->GetNetConnection()
		&& ServerWorldEpoch == HandshakeServerWorldEpoch && ReasonCode == 1)
	{
		RejectManifestHandshake(
			ServerWorldEpoch, TEXT("Client rejected the server manifest or received malformed data."));
	}
}

void AFlecsReplicationChannel::ClientDiscreteCommandResult_Implementation(
	uint64 WorldEpoch, uint32 Sequence, uint8 ResultCode)
{
	check(IsInGameThread());
	if (!HasAuthority() && WorldEpoch == HandshakeServerWorldEpoch && Sequence != 0
		&& ResultCode <= FlecsNetworkCommandResult::ExecutionResultExpired)
	{
		OnDiscreteCommandResultReceived.Broadcast(Sequence, ResultCode);
	}
}

void AFlecsReplicationChannel::ClientManifestResult_Implementation(
	uint64 ServerWorldEpoch, bool bCompatible, const FString& Reason)
{
	check(IsInGameThread());
	if (HasAuthority() || ServerWorldEpoch == 0 || ServerWorldEpoch != HandshakeServerWorldEpoch
		|| (!bManifestHandshakePending && (bCompatible || !bClientManifestValidated)))
	{
		return;
	}
	if (!bCompatible || !bLocalManifestCompared)
	{
		const FString FailureReason =
			bCompatible ? FString(TEXT("Local manifest comparison was not completed."))
						: (Reason.IsEmpty() ? FString(TEXT("The server rejected the peer session.")) : Reason);
		bManifestHandshakePending = false;
		bClientManifestValidated = false;
		bClientStateDispatchReady = false;
		IncomingManifest = FFlecsManifestAssembly();
		OutgoingManifest = FOutgoingManifest();
		bPendingManifestResult = true;
		PendingResultEpoch = ServerWorldEpoch;
		bPendingManifestResultCompatible = false;
		PendingManifestResultReason = FailureReason.Left(512);
		return;
	}
	bClientManifestValidated = true;
	bManifestHandshakePending = false;
	bPendingManifestResult = true;
	PendingResultEpoch = ServerWorldEpoch;
	bPendingManifestResultCompatible = true;
	PendingManifestResultReason.Reset();
	OnRep_ServerTickHeartbeat();
}

void AFlecsReplicationChannel::ResumeReceivedState()
{
	check(IsInGameThread());
	if (HasAuthority() || !bClientManifestValidated)
	{
		return;
	}
	bClientStateDispatchReady = true;
	HandleReplicatedSnapshot();
}

void AFlecsReplicationChannel::HandleManifestTimeout()
{
	RejectManifestHandshake(HandshakeServerWorldEpoch, TEXT("Manifest handshake timed out."));
}

void AFlecsReplicationChannel::SetChannelOwner(APlayerController* InOwner)
{
	check(IsInGameThread());
	if (!HasAuthority() || !IsValid(InOwner) || InOwner->GetWorld() != GetWorld())
	{
		return;
	}
	SetOwner(InOwner);
}

bool AFlecsReplicationChannel::PublishUpdateBatch(
	uint64 InWorldEpoch, const TArray<FFlecsReplicationRecord>& Records, uint32 InSimulationTick)
{
	check(IsInGameThread());
	LastUpdateBatchRejectionReason.Reset();
	const auto RejectBatch = [this](const TCHAR* Reason) {
		LastUpdateBatchRejectionReason = FString(Reason).Left(256);
		return false;
	};
	if (!HasAuthority() || !bServerManifestValidated || InWorldEpoch == 0 || InWorldEpoch != HandshakeServerWorldEpoch
		|| OutgoingSnapshotTransfer.bActive || Records.IsEmpty() || Records.Num() > MaxRecordsPerSnapshot
		|| (CurrentWorldEpoch == InWorldEpoch && CurrentPublishedSnapshotRevision == MAX_uint64))
	{
		return RejectBatch(
			TEXT("Update batch is not admissible for the current authority, epoch, transfer state, or record limit."));
	}
	const uint64 NewSnapshotRevision = CurrentWorldEpoch == InWorldEpoch ? CurrentPublishedSnapshotRevision + 1 : 1;
	if (InSimulationTick == 0)
	{
		return RejectBatch(TEXT("Update batch simulation tick is zero."));
	}
	if (CurrentWorldEpoch == InWorldEpoch && InSimulationTick < LastPublishedStateSimulationTick)
	{
		return RejectBatch(TEXT("Update batch state tick is older than the last accepted state batch."));
	}
	uint64 TotalPayloadBytes = 0;
	uint64 TotalWireFramingBytes = 0;
	TSet<FRecordKey> UniqueKeys;
	TMap<uint32, uint16> EntityGenerations;
	TMap<uint32, uint16> EntityDefinitions;
	TSet<uint32> DespawnedEntities;
	TSet<uint32> EntitiesWithComponents;
	UniqueKeys.Reserve(Records.Num());
	TMap<uint32, uint16> CandidateHighWatermarks;
	TMap<uint32, uint16> CandidateDefinitions;
	for (const FFlecsReplicationRecord& Record : Records)
	{
		if (!IsRecordBoundedAndValid(Record, InWorldEpoch))
		{
			return RejectBatch(TEXT("Update batch contains an invalid or out-of-epoch record."));
		}
		if (Record.SourceSimulationTick > InSimulationTick)
		{
			return RejectBatch(TEXT("Update batch contains a record newer than its batch tick."));
		}
		TotalPayloadBytes += static_cast<uint64>(Record.Payload.Num());
		TotalWireFramingBytes += RecordWireFramingBytes;
		if (TotalPayloadBytes + TotalWireFramingBytes > static_cast<uint64>(MaxSnapshotPayloadBytes))
		{
			return RejectBatch(TEXT("Update batch exceeds the payload and wire-framing budget."));
		}
		const uint16* ExistingGeneration = EntityGenerations.Find(Record.NetworkEntityId);
		const uint16* ExistingDefinition = EntityDefinitions.Find(Record.NetworkEntityId);
		if ((ExistingGeneration && *ExistingGeneration != Record.EntityGeneration)
			|| (ExistingDefinition && *ExistingDefinition != Record.DefinitionWireId))
		{
			return RejectBatch(TEXT("Update batch contains conflicting identity metadata for an entity."));
		}
		EntityGenerations.Add(Record.NetworkEntityId, Record.EntityGeneration);
		EntityDefinitions.Add(Record.NetworkEntityId, Record.DefinitionWireId);
		const uint16* PreviousGeneration =
			CurrentWorldEpoch == InWorldEpoch ? HighestPublishedGenerations.Find(Record.NetworkEntityId) : nullptr;
		const uint16* PreviousDefinition =
			CurrentWorldEpoch == InWorldEpoch ? HighestPublishedDefinitions.Find(Record.NetworkEntityId) : nullptr;
		if (PreviousGeneration && Record.EntityGeneration != *PreviousGeneration
			&& !IsGenerationNewer(Record.EntityGeneration, *PreviousGeneration))
		{
			return RejectBatch(TEXT("Update batch contains a stale entity generation."));
		}
		if (PreviousGeneration && Record.EntityGeneration == *PreviousGeneration && PreviousDefinition
			&& Record.DefinitionWireId != *PreviousDefinition)
		{
			return RejectBatch(TEXT("Update batch changes an entity definition without advancing its generation."));
		}
		CandidateHighWatermarks.Add(Record.NetworkEntityId, Record.EntityGeneration);
		CandidateDefinitions.Add(Record.NetworkEntityId, Record.DefinitionWireId);
		if (Record.Operation == 2 || Record.Operation == 4)
		{
			if (EntitiesWithComponents.Contains(Record.NetworkEntityId))
			{
				return RejectBatch(TEXT("Update batch mixes entity removal and component records."));
			}
			DespawnedEntities.Add(Record.NetworkEntityId);
		}
		else
		{
			if (DespawnedEntities.Contains(Record.NetworkEntityId))
			{
				return RejectBatch(TEXT("Update batch mixes entity removal and component records."));
			}
			EntitiesWithComponents.Add(Record.NetworkEntityId);
		}
		const FRecordKey Key = MakeKey(Record);
		if (UniqueKeys.Contains(Key))
		{
			return RejectBatch(TEXT("Update batch contains a duplicate record key."));
		}
		UniqueKeys.Add(Key);
	}

	if (CurrentWorldEpoch != InWorldEpoch)
	{
		HighestPublishedGenerations.Empty();
		HighestPublishedDefinitions.Empty();
		CurrentSimulationTick = 0;
		LastPublishedStateSimulationTick = 0;
		LastReceivedStateSimulationTick = 0;
		LastDiscreteCommandSequence = 0;
		PendingDiscreteCommandResults.Empty();
		CompletedDiscreteCommandResults.Empty();
		CompletedDiscreteCommandSequenceOrder.Empty();
		LastContinuousCommandSequence = 0;
		DiscreteCommandWindowStartSeconds = 0.0;
		ContinuousCommandWindowStartSeconds = 0.0;
		DiscreteCommandsInWindow = 0;
		ContinuousCommandsInWindow = 0;
	}
	for (const TPair<uint32, uint16>& HighWatermark : CandidateHighWatermarks)
	{
		HighestPublishedGenerations.Add(HighWatermark.Key, HighWatermark.Value);
	}
	for (const TPair<uint32, uint16>& Definition : CandidateDefinitions)
	{
		HighestPublishedDefinitions.Add(Definition.Key, Definition.Value);
	}
	CurrentWorldEpoch = InWorldEpoch;
	ReplicatedWorldEpoch = InWorldEpoch;
	CurrentPublishedSnapshotRevision = NewSnapshotRevision;
	CurrentSimulationTick = FMath::Max(CurrentSimulationTick, InSimulationTick);
	LastPublishedStateSimulationTick = InSimulationTick;
	if (ReplicatedServerTickHeartbeat.ServerWorldEpoch != InWorldEpoch
		|| InSimulationTick > ReplicatedServerTickHeartbeat.SimulationTick)
	{
		ReplicatedServerTickHeartbeat.ServerWorldEpoch = InWorldEpoch;
		ReplicatedServerTickHeartbeat.SimulationTick = InSimulationTick;
	}
	++NextSnapshotTransferId;
	if (NextSnapshotTransferId == 0)
	{
		++NextSnapshotTransferId;
	}
	OutgoingSnapshotTransfer = FOutgoingSnapshotTransfer();
	OutgoingSnapshotTransfer.bActive = true;
	OutgoingSnapshotTransfer.WorldEpoch = InWorldEpoch;
	OutgoingSnapshotTransfer.TransferId = NextSnapshotTransferId;
	OutgoingSnapshotTransfer.Revision = NewSnapshotRevision;
	OutgoingSnapshotTransfer.SimulationTick = InSimulationTick;
	OutgoingSnapshotTransfer.RecordCount = static_cast<uint16>(Records.Num());
	OutgoingSnapshotTransfer.Records = Records;
	for (int32 Index = 0; Index < OutgoingSnapshotTransfer.Records.Num(); ++Index)
	{
		FFlecsReplicationRecord& Record = OutgoingSnapshotTransfer.Records[Index];
		Record.WorldEpoch = InWorldEpoch;
		Record.SnapshotTransferId = NextSnapshotTransferId;
		Record.SnapshotRevision = NewSnapshotRevision;
		Record.SnapshotRecordIndex = static_cast<uint16>(Index);
		Record.SnapshotRecordCount = OutgoingSnapshotTransfer.RecordCount;
		Record.SnapshotSimulationTick = InSimulationTick;
	}
	PumpSnapshotWindow();
	return true;
}

uint64 AFlecsReplicationChannel::GetDiagnosticsAllocatedBytes() const
{
	uint64 Bytes = OutgoingManifest.Bytes.GetAllocatedSize() + OutgoingSnapshotTransfer.Records.GetAllocatedSize()
				 + IncomingSnapshotTransfer.Records.GetAllocatedSize()
				 + IncomingSnapshotTransfer.ReceivedIndices.GetAllocatedSize()
				 + IncomingManifest.Chunks.GetAllocatedSize() + IncomingManifest.ReceivedChunks.GetAllocatedSize()
				 + PendingManifestDescriptor.GetAllocatedSize() + ReplicatedState.Items.GetAllocatedSize()
				 + DiagnosticLastAcceptedState.Records.GetAllocatedSize();
	for (const TArray<uint8>& Chunk : IncomingManifest.Chunks)
		Bytes += Chunk.GetAllocatedSize();
	return Bytes;
}

FString AFlecsReplicationChannel::GetLastUpdateBatchRejectionReason() const
{
	check(IsInGameThread());
	return LastUpdateBatchRejectionReason.Left(256);
}

bool AFlecsReplicationChannel::IsUpdateBatchTransferActive() const
{
	return HasAuthority() && OutgoingSnapshotTransfer.bActive;
}

bool AFlecsReplicationChannel::CompleteReceivedSnapshotBatch(
	uint64 InWorldEpoch, uint64 InSnapshotRevision, bool bAccepted)
{
	check(IsInGameThread());
	if (HasAuthority() || !IncomingSnapshotTransfer.bActive || !IncomingSnapshotTransfer.bWaitingForWorker
		|| IncomingSnapshotTransfer.WorldEpoch != InWorldEpoch
		|| IncomingSnapshotTransfer.Revision != InSnapshotRevision)
	{
		return false;
	}
	const uint16 FinalRecordIndex = static_cast<uint16>(IncomingSnapshotTransfer.RecordCount - 1);
	if (bAccepted)
	{
		if (LastReceivedSnapshotEpoch == InWorldEpoch
			&& (InSnapshotRevision <= LastReceivedSnapshotRevision
				|| IncomingSnapshotTransfer.SimulationTick < LastReceivedStateSimulationTick))
		{
			bAccepted = false;
		}
		else
		{
			if (CurrentWorldEpoch != InWorldEpoch)
			{
				HighestReceivedGenerations.Empty();
				HighestReceivedDefinitions.Empty();
			}
			for (const FFlecsReplicationRecord& Record : IncomingSnapshotTransfer.Records)
			{
				const uint16* PreviousGeneration = HighestReceivedGenerations.Find(Record.NetworkEntityId);
				const uint16* PreviousDefinition = HighestReceivedDefinitions.Find(Record.NetworkEntityId);
				if (!PreviousGeneration || Record.EntityGeneration == *PreviousGeneration
					|| IsGenerationNewer(Record.EntityGeneration, *PreviousGeneration))
				{
					HighestReceivedGenerations.Add(Record.NetworkEntityId, Record.EntityGeneration);
					HighestReceivedDefinitions.Add(Record.NetworkEntityId, Record.DefinitionWireId);
				}
			}
			CurrentWorldEpoch = InWorldEpoch;
			LastReceivedSnapshotEpoch = InWorldEpoch;
			LastReceivedSnapshotRevision = InSnapshotRevision;
			LastReceivedStateSimulationTick = IncomingSnapshotTransfer.SimulationTick;
			CurrentSimulationTick = FMath::Max(CurrentSimulationTick, IncomingSnapshotTransfer.SimulationTick);
		}
	}
	if (bAccepted
		&& DiagnosticLastAcceptedState.Accept(
			InWorldEpoch, IncomingSnapshotTransfer.SimulationTick, IncomingSnapshotTransfer.Records))
	{
		if (UWorld* World = GetWorld())
			if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
				Diagnostics->RecordPayload(0, false, IncomingSnapshotTransfer.ReceivedPayloadBytes);
	}
	SendSnapshotWindowAcknowledgement(
		InWorldEpoch, IncomingSnapshotTransfer.TransferId, InSnapshotRevision, FinalRecordIndex, bAccepted);
	IncomingSnapshotTransfer = FIncomingSnapshotTransfer();
	return true;
}

void AFlecsReplicationChannel::PumpSnapshotWindow()
{
	check(IsInGameThread());
	if (!HasAuthority() || !OutgoingSnapshotTransfer.bActive || OutgoingSnapshotTransfer.bWaitingForAcknowledgement)
	{
		return;
	}
	const uint16 WindowStart = OutgoingSnapshotTransfer.NextRecordIndex;
	const uint16 WindowEnd = static_cast<uint16>(FMath::Min<int32>(
		OutgoingSnapshotTransfer.RecordCount, static_cast<int32>(WindowStart) + SnapshotWindowRecords));
	if (WindowStart >= WindowEnd)
	{
		FailSnapshotTransfer(OutgoingSnapshotTransfer.WorldEpoch, TEXT("Snapshot window range is invalid."));
		return;
	}
	if (!ReplicatedState.Items.IsEmpty())
	{
		ReplicatedState.Items.Reset();
		ReplicatedState.MarkArrayDirty();
	}
	for (uint16 Index = WindowStart; Index < WindowEnd; ++Index)
	{
		FFlecsReplicationRecord& Item = ReplicatedState.Items.AddDefaulted_GetRef();
		CopyState(Item, OutgoingSnapshotTransfer.Records[Index]);
		ReplicatedState.MarkItemDirty(Item);
	}
	OutgoingSnapshotTransfer.NextRecordIndex = WindowEnd;
	OutgoingSnapshotTransfer.AwaitingThroughRecordIndex = WindowEnd - 1;
	OutgoingSnapshotTransfer.bWaitingForAcknowledgement = true;
	OutgoingSnapshotTransfer.LastProgressSeconds = FPlatformTime::Seconds();
	ForceNetUpdate();
}

void AFlecsReplicationChannel::HandleSnapshotWindowAcknowledgement(
	uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision, uint16 HighestRecordIndex, bool bAccepted)
{
	check(IsInGameThread());
	if (!OutgoingSnapshotTransfer.bActive || !OutgoingSnapshotTransfer.bWaitingForAcknowledgement
		|| ServerWorldEpoch != OutgoingSnapshotTransfer.WorldEpoch || TransferId != OutgoingSnapshotTransfer.TransferId
		|| SnapshotRevision != OutgoingSnapshotTransfer.Revision
		|| HighestRecordIndex != OutgoingSnapshotTransfer.AwaitingThroughRecordIndex)
	{
		return;
	}
	if (!bAccepted)
	{
		FailSnapshotTransfer(ServerWorldEpoch, TEXT("Client rejected the snapshot window or worker batch."));
		return;
	}
	OutgoingSnapshotTransfer.bWaitingForAcknowledgement = false;
	OutgoingSnapshotTransfer.LastProgressSeconds = FPlatformTime::Seconds();
	if (OutgoingSnapshotTransfer.NextRecordIndex >= OutgoingSnapshotTransfer.RecordCount)
	{
		const uint64 CompletedRevision = OutgoingSnapshotTransfer.Revision;
		OutgoingSnapshotTransfer = FOutgoingSnapshotTransfer();
		if (!ReplicatedState.Items.IsEmpty())
		{
			ReplicatedState.Items.Reset();
			ReplicatedState.MarkArrayDirty();
			ForceNetUpdate();
		}
		OnSnapshotTransferFinished.Broadcast(ServerWorldEpoch, CompletedRevision, true);
		return;
	}
	PumpSnapshotWindow();
}

void AFlecsReplicationChannel::SendSnapshotWindowAcknowledgement(
	uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision, uint16 HighestRecordIndex, bool bAccepted)
{
	check(IsInGameThread());
	if (!HasAuthority() && ServerWorldEpoch != 0 && TransferId != 0 && SnapshotRevision != 0)
	{
		ServerAcknowledgeSnapshotWindow(ServerWorldEpoch, TransferId, SnapshotRevision, HighestRecordIndex, bAccepted);
	}
}

void AFlecsReplicationChannel::FailSnapshotTransfer(uint64 ServerWorldEpoch, const FString& Reason)
{
	check(IsInGameThread());
	if (HasAuthority())
	{
		if (!OutgoingSnapshotTransfer.bActive || ServerWorldEpoch != OutgoingSnapshotTransfer.WorldEpoch)
		{
			return;
		}
		UE_LOG(LogTemp, Warning, TEXT("Flecs server state transfer failed (epoch %llu, revision %llu): %s"),
			static_cast<unsigned long long>(ServerWorldEpoch),
			static_cast<unsigned long long>(OutgoingSnapshotTransfer.Revision), *Reason.Left(256));
		const uint64 FailedRevision = OutgoingSnapshotTransfer.Revision;
		OutgoingSnapshotTransfer = FOutgoingSnapshotTransfer();
		if (!ReplicatedState.Items.IsEmpty())
		{
			ReplicatedState.Items.Reset();
			ReplicatedState.MarkArrayDirty();
			ForceNetUpdate();
		}
		OnSnapshotTransferFinished.Broadcast(ServerWorldEpoch, FailedRevision, false);
		return;
	}
	if (!IncomingSnapshotTransfer.bActive || ServerWorldEpoch != IncomingSnapshotTransfer.WorldEpoch)
	{
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("Flecs client state transfer failed (epoch %llu, revision %llu): %s"),
		static_cast<unsigned long long>(ServerWorldEpoch),
		static_cast<unsigned long long>(IncomingSnapshotTransfer.Revision), *Reason.Left(256));
	const uint16 WindowEnd = static_cast<uint16>(FMath::Min<int32>(IncomingSnapshotTransfer.RecordCount,
		static_cast<int32>(IncomingSnapshotTransfer.NextAcknowledgementStartIndex) + SnapshotWindowRecords));
	const uint16 HighestRecordIndex = WindowEnd == 0 ? 0 : WindowEnd - 1;
	SendSnapshotWindowAcknowledgement(IncomingSnapshotTransfer.WorldEpoch, IncomingSnapshotTransfer.TransferId,
		IncomingSnapshotTransfer.Revision, HighestRecordIndex, false);
	IncomingSnapshotTransfer = FIncomingSnapshotTransfer();
}

void AFlecsReplicationChannel::HandleReplicatedSnapshot()
{
	check(IsInGameThread());
	if (HasAuthority() || !bClientManifestValidated || !bClientStateDispatchReady
		|| ReplicatedState.Items.Num() > SnapshotWindowRecords * 2)
	{
		return;
	}
	const double NowSeconds = FPlatformTime::Seconds();
	for (const FFlecsReplicationRecord& Record : ReplicatedState.Items)
	{
		if (Record.WorldEpoch == 0 || Record.WorldEpoch != HandshakeServerWorldEpoch || Record.SnapshotRevision == 0
			|| Record.SnapshotTransferId == 0 || Record.SnapshotRecordCount == 0
			|| Record.SnapshotRecordCount > static_cast<uint16>(MaxRecordsPerSnapshot)
			|| Record.SnapshotRecordIndex >= Record.SnapshotRecordCount || Record.SnapshotSimulationTick == 0)
		{
			continue;
		}
		if (LastReceivedSnapshotEpoch == Record.WorldEpoch && Record.SnapshotRevision <= LastReceivedSnapshotRevision)
		{
			continue;
		}
		if (LastReceivedSnapshotEpoch == Record.WorldEpoch
			&& Record.SnapshotSimulationTick < LastReceivedStateSimulationTick)
		{
			SendSnapshotWindowAcknowledgement(Record.WorldEpoch, Record.SnapshotTransferId, Record.SnapshotRevision,
				static_cast<uint16>(FMath::Min<int32>(Record.SnapshotRecordCount, SnapshotWindowRecords) - 1), false);
			return;
		}
		if (!IncomingSnapshotTransfer.bActive)
		{
			if (Record.SnapshotRecordIndex >= SnapshotWindowRecords)
			{
				SendSnapshotWindowAcknowledgement(Record.WorldEpoch, Record.SnapshotTransferId, Record.SnapshotRevision,
					static_cast<uint16>(FMath::Min<int32>(Record.SnapshotRecordCount, SnapshotWindowRecords) - 1),
					false);
				return;
			}
			IncomingSnapshotTransfer = FIncomingSnapshotTransfer();
			IncomingSnapshotTransfer.bActive = true;
			IncomingSnapshotTransfer.WorldEpoch = Record.WorldEpoch;
			IncomingSnapshotTransfer.TransferId = Record.SnapshotTransferId;
			IncomingSnapshotTransfer.Revision = Record.SnapshotRevision;
			IncomingSnapshotTransfer.SimulationTick = Record.SnapshotSimulationTick;
			IncomingSnapshotTransfer.RecordCount = Record.SnapshotRecordCount;
			IncomingSnapshotTransfer.LastProgressSeconds = NowSeconds;
			IncomingSnapshotTransfer.Records.SetNum(Record.SnapshotRecordCount);
			IncomingSnapshotTransfer.ReceivedIndices.Init(false, Record.SnapshotRecordCount);
		}
		if (IncomingSnapshotTransfer.bWaitingForWorker)
		{
			return;
		}
		if (Record.WorldEpoch != IncomingSnapshotTransfer.WorldEpoch
			|| Record.SnapshotTransferId != IncomingSnapshotTransfer.TransferId
			|| Record.SnapshotRevision != IncomingSnapshotTransfer.Revision
			|| Record.SnapshotSimulationTick != IncomingSnapshotTransfer.SimulationTick
			|| Record.SnapshotRecordCount != IncomingSnapshotTransfer.RecordCount)
		{
			FailSnapshotTransfer(
				IncomingSnapshotTransfer.WorldEpoch, TEXT("Mixed epoch, transfer, or revision in a FastArray window."));
			return;
		}
		const uint16 WindowStart = IncomingSnapshotTransfer.NextAcknowledgementStartIndex;
		const uint16 WindowEnd = static_cast<uint16>(FMath::Min<int32>(
			IncomingSnapshotTransfer.RecordCount, static_cast<int32>(WindowStart) + SnapshotWindowRecords));
		if (Record.SnapshotRecordIndex >= WindowEnd
			|| !IsRecordBoundedAndValid(Record, IncomingSnapshotTransfer.WorldEpoch)
			|| Record.SnapshotSimulationTick < Record.SourceSimulationTick)
		{
			FailSnapshotTransfer(
				IncomingSnapshotTransfer.WorldEpoch, TEXT("Invalid or out-of-order FastArray snapshot window record."));
			return;
		}
		if (IncomingSnapshotTransfer.ReceivedIndices[Record.SnapshotRecordIndex])
		{
			if (!HasSameSnapshotRecord(IncomingSnapshotTransfer.Records[Record.SnapshotRecordIndex], Record))
			{
				FailSnapshotTransfer(
					IncomingSnapshotTransfer.WorldEpoch, TEXT("Duplicate snapshot index carried different data."));
				return;
			}
			continue;
		}
		const uint64 PayloadBytes = static_cast<uint64>(Record.Payload.Num());
		const uint64 NewPayloadBytes = IncomingSnapshotTransfer.ReceivedPayloadBytes + PayloadBytes;
		const uint64 NewWireFramingBytes = IncomingSnapshotTransfer.ReceivedWireFramingBytes + RecordWireFramingBytes;
		if (NewPayloadBytes + NewWireFramingBytes > static_cast<uint64>(MaxSnapshotPayloadBytes))
		{
			FailSnapshotTransfer(
				IncomingSnapshotTransfer.WorldEpoch, TEXT("Snapshot batch exceeded its bounded encoded size."));
			return;
		}
		CopyState(IncomingSnapshotTransfer.Records[Record.SnapshotRecordIndex], Record);
		IncomingSnapshotTransfer.ReceivedIndices[Record.SnapshotRecordIndex] = true;
		++IncomingSnapshotTransfer.ReceivedRecordCount;
		IncomingSnapshotTransfer.ReceivedPayloadBytes = NewPayloadBytes;
		IncomingSnapshotTransfer.ReceivedWireFramingBytes = NewWireFramingBytes;
		IncomingSnapshotTransfer.LastProgressSeconds = NowSeconds;
	}
	if (!IncomingSnapshotTransfer.bActive || IncomingSnapshotTransfer.bWaitingForWorker)
	{
		return;
	}
	const uint16 WindowStart = IncomingSnapshotTransfer.NextAcknowledgementStartIndex;
	const uint16 WindowEnd = static_cast<uint16>(FMath::Min<int32>(
		IncomingSnapshotTransfer.RecordCount, static_cast<int32>(WindowStart) + SnapshotWindowRecords));
	for (uint16 Index = WindowStart; Index < WindowEnd; ++Index)
	{
		if (!IncomingSnapshotTransfer.ReceivedIndices[Index])
		{
			return;
		}
	}
	if (WindowEnd == IncomingSnapshotTransfer.RecordCount)
	{
		TSet<FRecordKey> UniqueKeys;
		TMap<uint32, uint16> EntityGenerations;
		TMap<uint32, uint16> EntityDefinitions;
		TSet<uint32> DespawnedEntities;
		TSet<uint32> EntitiesWithComponents;
		UniqueKeys.Reserve(IncomingSnapshotTransfer.Records.Num());
		for (const FFlecsReplicationRecord& Record : IncomingSnapshotTransfer.Records)
		{
			const uint16* PreviousGeneration = EntityGenerations.Find(Record.NetworkEntityId);
			const uint16* PreviousDefinition = EntityDefinitions.Find(Record.NetworkEntityId);
			if ((PreviousGeneration && *PreviousGeneration != Record.EntityGeneration)
				|| (PreviousDefinition && *PreviousDefinition != Record.DefinitionWireId))
			{
				FailSnapshotTransfer(IncomingSnapshotTransfer.WorldEpoch,
					TEXT("Snapshot batch mixes entity generations or definitions."));
				return;
			}
			EntityGenerations.Add(Record.NetworkEntityId, Record.EntityGeneration);
			EntityDefinitions.Add(Record.NetworkEntityId, Record.DefinitionWireId);
			if (Record.Operation == 2 || Record.Operation == 4)
			{
				if (EntitiesWithComponents.Contains(Record.NetworkEntityId))
				{
					FailSnapshotTransfer(IncomingSnapshotTransfer.WorldEpoch,
						TEXT("Entity despawn conflicts with another operation in its batch."));
					return;
				}
				DespawnedEntities.Add(Record.NetworkEntityId);
			}
			else
			{
				if (DespawnedEntities.Contains(Record.NetworkEntityId))
				{
					FailSnapshotTransfer(IncomingSnapshotTransfer.WorldEpoch,
						TEXT("Entity operation follows a despawn in the same batch."));
					return;
				}
				EntitiesWithComponents.Add(Record.NetworkEntityId);
			}
			if (UniqueKeys.Contains(MakeKey(Record)))
			{
				FailSnapshotTransfer(
					IncomingSnapshotTransfer.WorldEpoch, TEXT("Snapshot batch contains duplicate record identities."));
				return;
			}
			UniqueKeys.Add(MakeKey(Record));
			const uint16* HighestGeneration = HighestReceivedGenerations.Find(Record.NetworkEntityId);
			const uint16* HighestDefinition = HighestReceivedDefinitions.Find(Record.NetworkEntityId);
			if (LastReceivedSnapshotEpoch == Record.WorldEpoch && HighestGeneration
				&& Record.EntityGeneration != *HighestGeneration
				&& !IsGenerationNewer(Record.EntityGeneration, *HighestGeneration))
			{
				FailSnapshotTransfer(
					IncomingSnapshotTransfer.WorldEpoch, TEXT("Snapshot batch contains a stale entity generation."));
				return;
			}
			if (LastReceivedSnapshotEpoch == Record.WorldEpoch && HighestGeneration
				&& Record.EntityGeneration == *HighestGeneration && HighestDefinition
				&& Record.DefinitionWireId != *HighestDefinition)
			{
				FailSnapshotTransfer(IncomingSnapshotTransfer.WorldEpoch,
					TEXT("Snapshot batch changes a definition for an existing generation."));
				return;
			}
		}
		IncomingSnapshotTransfer.bWaitingForWorker = true;
		IncomingSnapshotTransfer.LastProgressSeconds = NowSeconds;
		TArray<FFlecsReplicationRecord> CompleteRecords = IncomingSnapshotTransfer.Records;
		OnCompleteStateBatchReceived.Broadcast(IncomingSnapshotTransfer.WorldEpoch, IncomingSnapshotTransfer.Revision,
			IncomingSnapshotTransfer.SimulationTick, CompleteRecords);
		return;
	}
	const uint16 HighestRecordIndex = WindowEnd - 1;
	SendSnapshotWindowAcknowledgement(IncomingSnapshotTransfer.WorldEpoch, IncomingSnapshotTransfer.TransferId,
		IncomingSnapshotTransfer.Revision, HighestRecordIndex, true);
	IncomingSnapshotTransfer.NextAcknowledgementStartIndex = WindowEnd;
	IncomingSnapshotTransfer.LastProgressSeconds = NowSeconds;
}

void AFlecsReplicationChannel::ServerAcknowledgeSnapshotWindow_Implementation(
	uint64 ServerWorldEpoch, uint32 TransferId, uint64 SnapshotRevision, uint16 HighestRecordIndex, bool bAccepted)
{
	check(IsInGameThread());
	const APlayerController* OwningController = Cast<APlayerController>(GetOwner());
	if (HasAuthority() && OwningController && OwningController->GetNetConnection())
	{
		HandleSnapshotWindowAcknowledgement(
			ServerWorldEpoch, TransferId, SnapshotRevision, HighestRecordIndex, bAccepted);
	}
}

void AFlecsReplicationChannel::OnRep_WorldEpoch()
{
	HandleReplicatedSnapshot();
}

void AFlecsReplicationChannel::OnRep_ServerTickHeartbeat()
{
	check(IsInGameThread());
	if (!HasAuthority() && bClientManifestValidated
		&& ReplicatedServerTickHeartbeat.ServerWorldEpoch == HandshakeServerWorldEpoch
		&& ReplicatedServerTickHeartbeat.SimulationTick > CurrentSimulationTick)
	{
		CurrentSimulationTick = ReplicatedServerTickHeartbeat.SimulationTick;
		OnServerTickHeartbeatReceived.Broadcast(
			ReplicatedServerTickHeartbeat.ServerWorldEpoch, ReplicatedServerTickHeartbeat.SimulationTick);
	}
}

void AFlecsReplicationChannel::RecordOutgoingCommand(const FFlecsCommandEnvelope& Envelope, bool bIsDiscrete)
{
	FFlecsLogicalSequence& Sequence = bIsDiscrete ? DiagnosticDiscreteSent : DiagnosticContinuousSent;
	if (!Envelope.Payload.IsValid() || !Sequence.Accept(Envelope.WorldEpoch, Envelope.Sequence))
		return;
	if (UWorld* World = GetWorld())
		if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
			Diagnostics->RecordPayload(1, true, Envelope.Payload.Num());
}

void AFlecsReplicationChannel::SubmitDiscreteCommand(const FFlecsCommandEnvelope& Envelope)
{
	check(IsInGameThread());
	if (!HasAuthority() && bClientManifestValidated)
	{
		RecordOutgoingCommand(Envelope, true);
		ServerSubmitDiscreteCommand(Envelope);
	}
}

void AFlecsReplicationChannel::SubmitContinuousCommand(const FFlecsCommandEnvelope& Envelope)
{
	check(IsInGameThread());
	if (!HasAuthority() && bClientManifestValidated)
	{
		RecordOutgoingCommand(Envelope, false);
		ServerSubmitContinuousCommand(Envelope);
	}
}

void AFlecsReplicationChannel::ServerSubmitDiscreteCommand_Implementation(const FFlecsCommandEnvelope& Envelope)
{
	HandleCommand(Envelope, true);
}

void AFlecsReplicationChannel::ServerSubmitContinuousCommand_Implementation(const FFlecsCommandEnvelope& Envelope)
{
	HandleCommand(Envelope, false);
}

void AFlecsReplicationChannel::HandleCommand(const FFlecsCommandEnvelope& Envelope, bool bIsDiscrete)
{
	check(IsInGameThread());
	const APlayerController* OwningController = Cast<APlayerController>(GetOwner());
	if (!OwningController || !OwningController->GetNetConnection())
	{
		return;
	}
	if (bIsDiscrete)
	{
		if (!bServerManifestValidated || Envelope.WorldEpoch == 0 || Envelope.WorldEpoch != CurrentWorldEpoch
			|| Envelope.Sequence == 0)
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedInvalid);
			return;
		}
		if (const uint8* PendingResult = PendingDiscreteCommandResults.Find(Envelope.Sequence))
		{
			SendDiscreteCommandResult(Envelope.WorldEpoch, Envelope.Sequence, *PendingResult);
			return;
		}
		if (const uint8* CompletedResult = CompletedDiscreteCommandResults.Find(Envelope.Sequence))
		{
			SendDiscreteCommandResult(Envelope.WorldEpoch, Envelope.Sequence, *CompletedResult);
			return;
		}
		const uint32 ExpectedSequence = LastDiscreteCommandSequence == MAX_uint32 ? 1 : LastDiscreteCommandSequence + 1;
		if (Envelope.Sequence != ExpectedSequence)
		{
			const uint32 BackwardDistance = LastDiscreteCommandSequence - Envelope.Sequence;
			const uint8 ResultCode = LastDiscreteCommandSequence != 0 && BackwardDistance < 0x80000000u
									   ? FlecsNetworkCommandResult::ExecutionResultExpired
									   : FlecsNetworkCommandResult::RejectedSequence;
			SendDiscreteCommandResult(Envelope.WorldEpoch, Envelope.Sequence, ResultCode);
			return;
		}
	}
	else if (!bServerManifestValidated || Envelope.WorldEpoch == 0 || Envelope.WorldEpoch != CurrentWorldEpoch
			 || Envelope.Sequence == 0)
	{
		return;
	}

	uint32& LastSequence = bIsDiscrete ? LastDiscreteCommandSequence : LastContinuousCommandSequence;
	uint16& CommandsInWindow = bIsDiscrete ? DiscreteCommandsInWindow : ContinuousCommandsInWindow;
	double& CommandWindowStartSeconds =
		bIsDiscrete ? DiscreteCommandWindowStartSeconds : ContinuousCommandWindowStartSeconds;
	const uint32 SequenceDistance = Envelope.Sequence - LastSequence;
	if (!bIsDiscrete && (SequenceDistance == 0 || SequenceDistance > 1024))
	{
		return;
	}

	const int32 RequestedTickDelta = static_cast<int32>(Envelope.RequestedSimulationTick - CurrentSimulationTick);
	if (Envelope.WorldEpoch == 0 || Envelope.WorldEpoch != CurrentWorldEpoch || Envelope.CommandWireId == 0
		|| Envelope.TargetNetworkEntityId == 0 || Envelope.TargetEntityGeneration == 0
		|| Envelope.RequestedSimulationTick == 0 || Envelope.Payload.Num() > MaxCommandPayloadBytes)
	{
		if (bIsDiscrete)
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedInvalid);
		}
		return;
	}
	if (RequestedTickDelta < -static_cast<int32>(MaxPastCommandTickWindow)
		|| RequestedTickDelta > static_cast<int32>(MaxFutureCommandTickWindow))
	{
		if (bIsDiscrete)
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedTickWindow);
		}
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds - CommandWindowStartSeconds >= 1.0)
	{
		CommandWindowStartSeconds = NowSeconds;
		CommandsInWindow = 0;
	}
	const uint16 RateLimit = bIsDiscrete ? MaxDiscreteCommandsPerSecond : MaxContinuousCommandsPerSecond;
	if (CommandsInWindow >= RateLimit)
	{
		if (bIsDiscrete)
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedRateLimit);
		}
		return;
	}

	if (bIsDiscrete)
	{
		if (PendingDiscreteCommandResults.Num() >= MaxPendingDiscreteCommandResults)
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedWorkerUnavailable);
			return;
		}
		if (!OnDiscreteCommandAdmission || !OnDiscreteCommandAdmission(Envelope))
		{
			SendDiscreteCommandResult(
				Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::RejectedWorkerUnavailable);
			return;
		}
		LastSequence = Envelope.Sequence;
		PendingDiscreteCommandResults.Add(Envelope.Sequence, FlecsNetworkCommandResult::AcceptedForWorker);
		++CommandsInWindow;
		if (UWorld* World = GetWorld())
			if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
				Diagnostics->RecordPayload(1, false, Envelope.Payload.Num());
		OnDiscreteCommandReceived.Broadcast(Envelope);
		SendDiscreteCommandResult(Envelope.WorldEpoch, Envelope.Sequence, FlecsNetworkCommandResult::AcceptedForWorker);
	}
	else
	{
		LastSequence = Envelope.Sequence;
		++CommandsInWindow;
		if (UWorld* World = GetWorld())
			if (auto* Diagnostics = World->GetSubsystem<UFlecsNetworkDiagnosticsSubsystem>())
				Diagnostics->RecordPayload(1, false, Envelope.Payload.Num());
		OnContinuousCommandReceived.Broadcast(Envelope);
	}
}

void AFlecsReplicationChannel::SendDiscreteCommandResult(uint64 WorldEpoch, uint32 Sequence, uint8 ResultCode)
{
	check(IsInGameThread());
	if (HasAuthority() && WorldEpoch != 0 && Sequence != 0)
	{
		ClientDiscreteCommandResult(WorldEpoch, Sequence, ResultCode);
	}
}

bool AFlecsReplicationChannel::ReportDiscreteCommandExecutionResult(
	uint64 InWorldEpoch, uint32 Sequence, uint8 ResultCode)
{
	check(IsInGameThread());
	if (!HasAuthority() || InWorldEpoch == 0 || InWorldEpoch != CurrentWorldEpoch || Sequence == 0
		|| ResultCode < FlecsNetworkCommandResult::ExecutionAccepted
		|| ResultCode > FlecsNetworkCommandResult::ExecutionFailed || !PendingDiscreteCommandResults.Remove(Sequence))
	{
		return false;
	}
	CompletedDiscreteCommandResults.Add(Sequence, ResultCode);
	CompletedDiscreteCommandSequenceOrder.Add(Sequence);
	while (CompletedDiscreteCommandSequenceOrder.Num() > MaxCachedDiscreteCommandResults)
	{
		const uint32 ExpiredSequence = CompletedDiscreteCommandSequenceOrder[0];
		CompletedDiscreteCommandSequenceOrder.RemoveAt(0, 1, EAllowShrinking::No);
		CompletedDiscreteCommandResults.Remove(ExpiredSequence);
	}
	SendDiscreteCommandResult(InWorldEpoch, Sequence, ResultCode);
	return true;
}

bool AFlecsReplicationChannel::PublishServerTickHeartbeat(uint64 InWorldEpoch, uint32 InSimulationTick)
{
	check(IsInGameThread());
	if (!HasAuthority() || !bServerManifestValidated || InWorldEpoch == 0 || InWorldEpoch != HandshakeServerWorldEpoch
		|| InSimulationTick == 0 || (CurrentWorldEpoch == InWorldEpoch && InSimulationTick < CurrentSimulationTick)
		|| (ReplicatedServerTickHeartbeat.ServerWorldEpoch == InWorldEpoch
			&& InSimulationTick <= ReplicatedServerTickHeartbeat.SimulationTick))
	{
		return false;
	}
	CurrentWorldEpoch = InWorldEpoch;
	CurrentSimulationTick = InSimulationTick;
	ReplicatedWorldEpoch = InWorldEpoch;
	ReplicatedServerTickHeartbeat.ServerWorldEpoch = InWorldEpoch;
	ReplicatedServerTickHeartbeat.SimulationTick = InSimulationTick;
	ForceNetUpdate();
	return true;
}

bool AFlecsReplicationChannel::IsRecordBoundedAndValid(
	const FFlecsReplicationRecord& Record, uint64 ExpectedEpoch) const
{
	return Record.WorldEpoch != 0 && Record.WorldEpoch == ExpectedEpoch && Record.NetworkEntityId != 0
		&& Record.EntityGeneration != 0 && Record.DefinitionWireId != 0 && Record.SourceSimulationTick != 0
		&& Record.Operation <= 4
		&& ((Record.Operation == 0 || Record.Operation == 1) ? Record.ComponentWireId != 0
															 : Record.ComponentWireId == 0)
		&& Record.Payload.IsValid() && Record.Payload.Num() <= MaxPayloadBytesPerRecord
		&& (Record.Operation == 0 || Record.Payload.IsEmpty());
}

void AFlecsReplicationChannel::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(AFlecsReplicationChannel, ReplicatedState, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AFlecsReplicationChannel, ReplicatedWorldEpoch, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AFlecsReplicationChannel, ReplicatedServerTickHeartbeat, COND_OwnerOnly);
}
