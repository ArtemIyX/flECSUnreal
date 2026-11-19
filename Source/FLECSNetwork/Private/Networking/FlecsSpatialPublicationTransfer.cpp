#include "Networking/FlecsSpatialPublicationTransfer.h"
#include "Misc/ScopeLock.h"

namespace FlecsSpatialTransfer
{
bool FMailbox::IsPending() const
{
	FScopeLock Lock(&Mutex);
	return Pending.IsSet();
}

bool FMailbox::TryPublish(FChunk&& Chunk)
{
	FScopeLock Lock(&Mutex);
	if (Pending.IsSet())
		return false;
	Pending.Emplace(MoveTemp(Chunk));
	return true;
}

bool FMailbox::TryReceive(FChunk& OutChunk)
{
	FScopeLock Lock(&Mutex);
	if (!Pending.IsSet())
		return false;
	OutChunk = MoveTemp(Pending.GetValue());
	Pending.Reset();
	return true;
}

uint64 FMailbox::GetTrackedArrayBytes() const
{
	FScopeLock Lock(&Mutex);
	return Pending.IsSet() ? static_cast<uint64>(Pending.GetValue().Identities.GetAllocatedSize())
								+ static_cast<uint64>(Pending.GetValue().Metadata.GetAllocatedSize()) : 0;
}

void FMailbox::Reset()
{
	FScopeLock Lock(&Mutex);
	Pending.Reset();
}

bool FHeartbeat::Publish(uint64 LocalEpoch, uint32 SimulationTick)
{
	FScopeLock Lock(&Mutex);
	if (LocalEpoch == 0 || SimulationTick == 0 || (Epoch != 0 && Epoch != LocalEpoch)
		|| (Epoch == LocalEpoch && SimulationTick <= Tick))
		return false;
	Epoch = LocalEpoch;
	Tick = SimulationTick;
	return true;
}

bool FHeartbeat::Read(uint64 ExpectedLocalEpoch, uint32& OutSimulationTick) const
{
	FScopeLock Lock(&Mutex);
	OutSimulationTick = 0;
	if (ExpectedLocalEpoch == 0 || ExpectedLocalEpoch != Epoch || Tick == 0)
		return false;
	OutSimulationTick = Tick;
	return true;
}

void FHeartbeat::Reset()
{
	FScopeLock Lock(&Mutex);
	Epoch = 0;
	Tick = 0;
}

bool FAssembler::Add(FChunk&& Chunk, uint64 ExpectedLocalEpoch, uint64 ExpectedServerEpoch, int32 Capacity)
{
	if (Capacity < 1 || Capacity > MaxEntities || Chunk.LocalEpoch == 0 || Chunk.LocalEpoch != ExpectedLocalEpoch
		|| Chunk.ServerEpoch == 0 || Chunk.ServerEpoch != ExpectedServerEpoch || Chunk.Generation == 0
		|| Chunk.SimulationTick == 0 || Chunk.TotalIdentities < 0 || Chunk.TotalIdentities > Capacity
		|| Chunk.TotalMetadata < 0 || Chunk.TotalMetadata > Chunk.TotalIdentities
		|| Chunk.Identities.Num() > MaxChunkItems || Chunk.Metadata.Num() > MaxChunkItems)
	{
		return false;
	}
	if (Chunk.Generation > Generation)
	{
		if (Chunk.Index != 0)
			return false;
		Reset();
		LocalEpoch = Chunk.LocalEpoch;
		ServerEpoch = Chunk.ServerEpoch;
		Generation = Chunk.Generation;
		Tick = Chunk.SimulationTick;
		TotalIdentities = Chunk.TotalIdentities;
		TotalMetadata = Chunk.TotalMetadata;
		Identities.Reserve(TotalIdentities);
		Metadata.Reserve(TotalMetadata);
	}
	if (Chunk.Generation != Generation || Chunk.LocalEpoch != LocalEpoch || Chunk.ServerEpoch != ServerEpoch
		|| bComplete || bConsumed || Chunk.Index != NextIndex || Chunk.SimulationTick != Tick
		|| Chunk.TotalIdentities != TotalIdentities || Chunk.TotalMetadata != TotalMetadata
		|| Identities.Num() + Chunk.Identities.Num() > TotalIdentities
		|| Metadata.Num() + Chunk.Metadata.Num() > TotalMetadata
		|| (Chunk.bComplete && (Identities.Num() + Chunk.Identities.Num() != TotalIdentities
			|| Metadata.Num() + Chunk.Metadata.Num() != TotalMetadata))
		|| (!Chunk.bComplete && Chunk.Identities.IsEmpty() && Chunk.Metadata.IsEmpty()))
		return false;
	Identities.Append(MoveTemp(Chunk.Identities));
	Metadata.Append(MoveTemp(Chunk.Metadata));
	++NextIndex;
	if (Chunk.bComplete)
	{
		bComplete = true;
	}
	return true;
}

bool FAssembler::Take(TArray<FlecsSimulation::FNetworkEntityIdentity>& OutIdentities,
	TArray<FlecsSimulation::FSpatialEntityMetadata>& OutMetadata, uint32& OutTick)
{
	OutIdentities.Reset();
	OutMetadata.Reset();
	OutTick = 0;
	if (!bComplete)
		return false;
	// No partial or conflicting identity set may reach relevance selection.
	TMap<uint32, const FlecsSimulation::FNetworkEntityIdentity*> Live;
	Live.Reserve(Identities.Num());
	for (const FlecsSimulation::FNetworkEntityIdentity& Identity : Identities)
	{
		if (!Identity.IsValid() || Identity.ServerWorldEpoch != ServerEpoch || Live.Contains(Identity.NetworkEntityId))
		{
			bComplete = false;
			bConsumed = true;
			bRejected = true;
			return false;
		}
		Live.Add(Identity.NetworkEntityId, &Identity);
	}
	TSet<uint32> SeenMetadata;
	SeenMetadata.Reserve(Metadata.Num());
	for (const FlecsSimulation::FSpatialEntityMetadata& Item : Metadata)
	{
		const FlecsSimulation::FNetworkEntityIdentity* const* Found = Live.Find(Item.Identity.NetworkEntityId);
		if (!Found || Item.Identity.ServerWorldEpoch != ServerEpoch || Item.Identity.Generation != (*Found)->Generation
			|| Item.Identity.DefinitionStableId != (*Found)->DefinitionStableId
			|| SeenMetadata.Contains(Item.Identity.NetworkEntityId))
		{
			bComplete = false;
			bConsumed = true;
			bRejected = true;
			return false;
		}
		SeenMetadata.Add(Item.Identity.NetworkEntityId);
	}
	OutIdentities = MoveTemp(Identities);
	OutMetadata = MoveTemp(Metadata);
	OutTick = Tick;
	// Keep the generation high-water mark to reject delayed chunks.
	bComplete = false;
	bConsumed = true;
	return true;
}

uint64 FAssembler::GetTrackedArrayBytes() const
{
	return static_cast<uint64>(Identities.GetAllocatedSize()) + static_cast<uint64>(Metadata.GetAllocatedSize());
}

void FAssembler::Reset()
{
	Generation = 0;
	LocalEpoch = 0;
	ServerEpoch = 0;
	Tick = 0;
	NextIndex = 0;
	TotalIdentities = 0;
	TotalMetadata = 0;
	bComplete = false;
	bConsumed = false;
	bRejected = false;
	Identities.Reset();
	Metadata.Reset();
}
}
