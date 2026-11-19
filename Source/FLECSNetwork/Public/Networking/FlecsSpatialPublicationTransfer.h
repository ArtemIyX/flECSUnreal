#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Data/Structs/FlecsNetworkEntityIdentity.h"
#include "Data/Structs/FlecsSpatialEntityMetadata.h"
#include "Networking/FlecsNetworkLimits.h"

namespace FlecsSpatialTransfer
{
/** Worker-to-game-thread transfer; it is not sent over the network. */
inline constexpr int32 MaxEntities = FlecsNetworkLimits::MaxEntityCapacity;
inline constexpr int32 MaxChunkItems = 8192;

struct FChunk
{
	uint64 LocalEpoch = 0;
	uint64 ServerEpoch = 0;
	uint64 Generation = 0;
	uint32 SimulationTick = 0;
	uint32 Index = 0;
	int32 TotalIdentities = 0;
	int32 TotalMetadata = 0;
	bool bComplete = false;
	TArray<FlecsSimulation::FNetworkEntityIdentity> Identities;
	TArray<FlecsSimulation::FSpatialEntityMetadata> Metadata;
};

/** Single-producer, single-consumer handoff; receiving a chunk releases its slot. */
class FLECSNETWORK_API FMailbox
{
public:
	bool IsPending() const;
	bool TryPublish(FChunk&& Chunk);
	bool TryReceive(FChunk& OutChunk);
	/** Allocated chunk array storage; excludes strings and allocator overhead. */
	uint64 GetTrackedArrayBytes() const;
	void Reset();

private:
	mutable FCriticalSection Mutex;
	TOptional<FChunk> Pending;
};

/** Latest worker tick, copied independently from snapshot chunks. */
class FLECSNETWORK_API FHeartbeat
{
public:
	bool Publish(uint64 LocalEpoch, uint32 SimulationTick);
	bool Read(uint64 ExpectedLocalEpoch, uint32& OutSimulationTick) const;
	void Reset();

private:
	mutable FCriticalSection Mutex;
	uint64 Epoch = 0;
	uint32 Tick = 0;
};

/** Game-thread assembly. Only a fully validated completed transfer can be consumed. */
class FLECSNETWORK_API FAssembler
{
public:
	bool Add(FChunk&& Chunk, uint64 ExpectedLocalEpoch, uint64 ExpectedServerEpoch, int32 Capacity);
	bool Take(TArray<FlecsSimulation::FNetworkEntityIdentity>& OutIdentities,
		TArray<FlecsSimulation::FSpatialEntityMetadata>& OutMetadata, uint32& OutTick);
	bool WasRejected() const { return bRejected; }
	/** Game-thread array storage only; excludes strings and temporary validation maps. */
	uint64 GetTrackedArrayBytes() const;
	void Reset();

private:
	uint64 LocalEpoch = 0;
	uint64 ServerEpoch = 0;
	uint64 Generation = 0;
	uint32 Tick = 0;
	uint32 NextIndex = 0;
	int32 TotalIdentities = 0;
	int32 TotalMetadata = 0;
	bool bComplete = false;
	bool bConsumed = false;
	bool bRejected = false;
	TArray<FlecsSimulation::FNetworkEntityIdentity> Identities;
	TArray<FlecsSimulation::FSpatialEntityMetadata> Metadata;
};
}
