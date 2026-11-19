#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Networking/FlecsSpatialPublicationTransfer.h"
#include "Relevance/FlecsRelevanceScheduler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsSpatialPublicationTransferTest, "Flecs.Network.SpatialPublicationTransfer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsSpatialPublicationTransferTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace FlecsSpatialTransfer;
	FAssembler Assembler;
	auto MakeChunk = [](uint64 Generation, uint32 Index, bool bComplete) {
		FChunk Chunk;
		Chunk.LocalEpoch = 2;
		Chunk.ServerEpoch = 3;
		Chunk.Generation = Generation;
		Chunk.SimulationTick = 4;
		Chunk.Index = Index;
		Chunk.TotalIdentities = 2;
		Chunk.TotalMetadata = 0;
		Chunk.bComplete = bComplete;
		Chunk.Identities.Add({3, Index + 1, 1, TEXT("test:entity")});
		return Chunk;
	};
	TArray<FlecsSimulation::FNetworkEntityIdentity> Identities;
	TArray<FlecsSimulation::FSpatialEntityMetadata> Metadata;
	uint32 Tick = 0;
	FMailbox Mailbox;
	TestTrue(TEXT("Empty mailbox accepts a chunk"), Mailbox.TryPublish(MakeChunk(1, 0, false)));
	TestTrue(TEXT("Published chunk remains pending"), Mailbox.IsPending());
	TestTrue(TEXT("Pending chunk reports tracked array storage"), Mailbox.GetTrackedArrayBytes() > 0);
	TestFalse(TEXT("Full mailbox applies backpressure"), Mailbox.TryPublish(MakeChunk(1, 1, true)));
	FHeartbeat Heartbeat;
	uint32 HeartbeatTick = 0;
	TestTrue(TEXT("Heartbeat publishes while the chunk slot is full"), Heartbeat.Publish(2, 10));
	TestTrue(TEXT("Heartbeat advances independently of chunk acknowledgement"), Heartbeat.Publish(2, 11));
	TestTrue(TEXT("Current epoch reads the latest heartbeat"), Heartbeat.Read(2, HeartbeatTick));
	TestEqual(TEXT("Latest heartbeat tick is copied"), HeartbeatTick, 11u);
	TestFalse(TEXT("Heartbeat cannot regress"), Heartbeat.Publish(2, 10));
	TestFalse(TEXT("A different epoch cannot read the heartbeat"), Heartbeat.Read(3, HeartbeatTick));
	Heartbeat.Reset();
	TestFalse(TEXT("Teardown clears the heartbeat"), Heartbeat.Read(2, HeartbeatTick));
	FChunk Received;
	TestTrue(TEXT("Receiving acknowledges the slot"), Mailbox.TryReceive(Received));
	TestEqual(TEXT("Acknowledged mailbox releases tracked array storage"), Mailbox.GetTrackedArrayBytes(), uint64(0));
	TestEqual(TEXT("Backpressure preserves the original chunk"), Received.Index, 0u);
	TestFalse(TEXT("Mailbox has no second chunk"), Mailbox.TryReceive(Received));
	TestTrue(TEXT("Producer resumes after acknowledgement"), Mailbox.TryPublish(MakeChunk(1, 1, true)));
	Mailbox.Reset();
	TestFalse(TEXT("Reset discards pending chunks"), Mailbox.IsPending());
	TestFalse(TEXT("A retired world's chunk is unavailable after reset"), Mailbox.TryReceive(Received));
	FChunk NextWorld = MakeChunk(2, 0, false);
	NextWorld.LocalEpoch = 4;
	TestTrue(TEXT("Mailbox can be reused for a new world"), Mailbox.TryPublish(MoveTemp(NextWorld)));
	TestTrue(TEXT("New world chunk arrives"), Mailbox.TryReceive(Received));
	TestEqual(TEXT("New world epoch is preserved"), Received.LocalEpoch, uint64(4));
	TestFalse(TEXT("Out-of-order first chunk rejected"), Assembler.Add(MakeChunk(1, 1, true), 2, 3, 100000));
	FChunk Excess = MakeChunk(1, 0, false);
	Excess.TotalIdentities = MaxEntities + 1;
	TestFalse(TEXT("Snapshot above hard capacity is rejected"), Assembler.Add(MoveTemp(Excess), 2, 3, MaxEntities));
	TestTrue(TEXT("First chunk accepted"), Assembler.Add(MakeChunk(1, 0, false), 2, 3, 100000));
	TestTrue(TEXT("Partial assembly reports tracked array storage"), Assembler.GetTrackedArrayBytes() > 0);
	TestFalse(TEXT("Incomplete snapshot cannot be consumed"), Assembler.Take(Identities, Metadata, Tick));
	TestFalse(TEXT("Duplicate chunk rejected"), Assembler.Add(MakeChunk(1, 0, false), 2, 3, 100000));
	FChunk Conflict = MakeChunk(1, 0, false);
	Conflict.Identities[0].NetworkEntityId = 99;
	TestFalse(TEXT("Conflicting duplicate rejected"), Assembler.Add(MoveTemp(Conflict), 2, 3, 100000));
	FChunk BadCount = MakeChunk(1, 1, true);
	BadCount.TotalIdentities = 3;
	TestFalse(TEXT("Inconsistent completion count rejected"), Assembler.Add(MoveTemp(BadCount), 2, 3, 100000));
	TestFalse(TEXT("Stale epoch rejected"), Assembler.Add(MakeChunk(1, 1, true), 9, 3, 100000));
	TestTrue(TEXT("Final chunk accepted"), Assembler.Add(MakeChunk(1, 1, true), 2, 3, 100000));
	TestTrue(TEXT("Complete snapshot available"), Assembler.Take(Identities, Metadata, Tick));
	TestEqual(TEXT("Both identities assembled"), Identities.Num(), 2);
	TestEqual(TEXT("Original tick retained"), Tick, 4u);
	TestFalse(TEXT("Consumed generation cannot be replayed"), Assembler.Add(MakeChunk(1, 0, false), 2, 3, 100000));
	TestTrue(TEXT("New generation starts"), Assembler.Add(MakeChunk(2, 0, false), 2, 3, 100000));
	TestTrue(TEXT("Newer generation supersedes partial assembly"), Assembler.Add(MakeChunk(3, 0, false), 2, 3, 100000));
	TestFalse(TEXT("Old generation remains stale"), Assembler.Add(MakeChunk(2, 1, true), 2, 3, 100000));
	TestTrue(TEXT("New generation completes"), Assembler.Add(MakeChunk(3, 1, true), 2, 3, 100000));
	TestTrue(TEXT("New generation can be consumed"), Assembler.Take(Identities, Metadata, Tick));
	FChunk DuplicateIds = MakeChunk(4, 0, true);
	const FlecsSimulation::FNetworkEntityIdentity DuplicateIdentity = DuplicateIds.Identities[0];
	DuplicateIds.Identities.Add(DuplicateIdentity);
	TestTrue(TEXT("Duplicate-ID chunk is assembled"), Assembler.Add(MoveTemp(DuplicateIds), 2, 3, 100000));
	TestFalse(TEXT("Duplicate identities cannot become a complete snapshot"), Assembler.Take(Identities, Metadata, Tick));
	TestTrue(TEXT("Invalid completed identity set reports rejection"), Assembler.WasRejected());
	TestTrue(TEXT("Rejected completion clears the output identities"), Identities.IsEmpty());
	TestEqual(TEXT("Rejected completion clears the output tick"), Tick, 0u);
	FChunk MismatchedMetadata = MakeChunk(5, 0, true);
	MismatchedMetadata.TotalIdentities = 1;
	MismatchedMetadata.TotalMetadata = 1;
	FlecsSimulation::FSpatialEntityMetadata InvalidMetadata;
	InvalidMetadata.Identity = MismatchedMetadata.Identities[0];
	InvalidMetadata.Identity.Generation = 2;
	MismatchedMetadata.Metadata.Add(MoveTemp(InvalidMetadata));
	TestTrue(TEXT("Mismatched metadata chunk is assembled"), Assembler.Add(MoveTemp(MismatchedMetadata), 2, 3, 100000));
	TestFalse(TEXT("Metadata for the wrong generation cannot be consumed"), Assembler.Take(Identities, Metadata, Tick));
	Assembler.Reset();
	TestFalse(TEXT("Reset discards completed transfer"), Assembler.Take(Identities, Metadata, Tick));
	// Reset retains reusable array capacity; it remains accounted rather than being reported as freed.
	TestTrue(TEXT("Retained assembly array storage remains measurable"), Assembler.GetTrackedArrayBytes() > 0);
	Assembler.Reset();
	for (int32 Offset = 0, Index = 0; Offset < MaxEntities; Offset += MaxChunkItems, ++Index)
	{
		FChunk Chunk;
		Chunk.LocalEpoch = 2;
		Chunk.ServerEpoch = 3;
		Chunk.Generation = 1;
		Chunk.SimulationTick = 10;
		Chunk.Index = static_cast<uint32>(Index);
		Chunk.TotalIdentities = MaxEntities;
		const int32 End = FMath::Min(Offset + MaxChunkItems, MaxEntities);
		Chunk.bComplete = End == MaxEntities;
		for (int32 Id = Offset; Id < End; ++Id)
			Chunk.Identities.Add({3, static_cast<uint32>(Id + 1), 1, TEXT("test:entity")});
		if (!TestTrue(TEXT("Large snapshot chunk accepted"), Assembler.Add(MoveTemp(Chunk), 2, 3, MaxEntities)))
			return false;
	}
	TestTrue(TEXT("Maximum-sized snapshot completes"), Assembler.Take(Identities, Metadata, Tick));
	TestEqual(TEXT("Maximum-sized snapshot retains all identities"), Identities.Num(), MaxEntities);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsSpatialPublicationRelevanceTest,
	"Flecs.Network.SpatialPublicationRelevance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsSpatialPublicationRelevanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace FlecsSpatialTransfer;
	FAssembler Assembler;
	FFlecsSpatialSnapshot Snapshot;
	FFlecsSpatialIndex Index;
	FFlecsPerConnectionScheduler Scheduler;
	FFlecsConnectionInterest Interest;
	Interest.ConnectionId = 1;
	Interest.Tiers = {{100.0f, 10.0f}};
	FFlecsRelevanceBudget Budget;
	FFlecsRelevanceSelection Selection;
	TArray<FlecsSimulation::FNetworkEntityIdentity> Identities;
	TArray<FlecsSimulation::FSpatialEntityMetadata> Metadata;
	uint32 Tick = 0;
	auto MakeChunk = [](uint64 Generation, uint32 IndexValue, uint32 EntityId, int32 Total, bool bComplete) {
		FChunk Chunk;
		Chunk.LocalEpoch = 2;
		Chunk.ServerEpoch = 3;
		Chunk.Generation = Generation;
		Chunk.SimulationTick = static_cast<uint32>(Generation);
		Chunk.Index = IndexValue;
		Chunk.TotalIdentities = Total;
		Chunk.TotalMetadata = Total;
		Chunk.bComplete = bComplete;
		FlecsSimulation::FNetworkEntityIdentity Identity{3, EntityId, 1, TEXT("test:entity")};
		Chunk.Identities.Add(Identity);
		FlecsSimulation::FSpatialEntityMetadata Item;
		Item.Identity = Identity;
		Item.Revision = 1;
		Item.EstimatedUpdateBytes = 32;
		Chunk.Metadata.Add(MoveTemp(Item));
		return Chunk;
	};
	auto InstallCompletedSnapshot = [&]() {
		if (!Assembler.Take(Identities, Metadata, Tick))
			return false;
		Snapshot = {};
		Snapshot.WorldEpoch = 3;
		Snapshot.SimulationTick = Tick;
		Snapshot.bHasCompleteLiveEntitySet = true;
		for (const FlecsSimulation::FNetworkEntityIdentity& Identity : Identities)
			Snapshot.LiveEntityKeys.Add({Identity.ServerWorldEpoch, Identity.NetworkEntityId, Identity.Generation});
		for (const FlecsSimulation::FSpatialEntityMetadata& Item : Metadata)
		{
			FFlecsSpatialEntity Entity;
			Entity.Key = {Item.Identity.ServerWorldEpoch, Item.Identity.NetworkEntityId, Item.Identity.Generation};
			Entity.Revision = Item.Revision;
			Entity.EstimatedUpdateBytes = Item.EstimatedUpdateBytes;
			Snapshot.Entities.Add(MoveTemp(Entity));
		}
		return Index.Build(Snapshot, 100.0f);
	};
	if (!TestTrue(TEXT("Initial generation is accepted"), Assembler.Add(MakeChunk(1, 0, 1, 1, true), 2, 3, 10))
		|| !TestTrue(TEXT("Initial snapshot is installed"), InstallCompletedSnapshot()))
		return false;
	TestTrue(TEXT("Spatial index reports tracked container allocations"), Index.GetTrackedContainerBytes() > 0);
	TestTrue(TEXT("Initial entity selected"), Scheduler.Select(Index, Interest, 1.0, Budget, Selection, 1));
	if (!TestEqual(TEXT("Initial entry count"), Selection.Records.Num(), 1))
		return false;
	TestTrue(TEXT("Initial entry acknowledged"), Scheduler.CompleteSelection(Selection.SelectionId, true, 1.0));
	TestTrue(TEXT("Next generation begins"), Assembler.Add(MakeChunk(2, 0, 2, 2, false), 2, 3, 10));
	TestFalse(TEXT("Incomplete generation is not installed"), InstallCompletedSnapshot());
	TestTrue(TEXT("Previous snapshot stays usable"), Scheduler.Select(Index, Interest, 2.0, Budget, Selection, 1));
	TestEqual(TEXT("Incomplete generation causes no false exit"), Selection.Records.Num(), 0);
	TestTrue(TEXT("Next generation completes"), Assembler.Add(MakeChunk(2, 1, 3, 2, true), 2, 3, 10));
	TestTrue(TEXT("Complete generation is installed"), InstallCompletedSnapshot());
	TestTrue(TEXT("Selection includes actual exit"), Scheduler.Select(Index, Interest, 3.0, Budget, Selection, 1));
	if (!TestEqual(TEXT("Capacity remains occupied until exit acknowledgment"), Selection.Records.Num(), 1))
		return false;
	TestTrue(TEXT("Old entity exits"), Selection.Records[0].Key.EntityId == 1
		&& Selection.Records[0].Transition == EFlecsRelevanceTransition::Exit);
	TestTrue(TEXT("Exit acknowledged"), Scheduler.CompleteSelection(Selection.SelectionId, true, 3.0));
	TestTrue(TEXT("Deferred entry can now be selected"), Scheduler.Select(Index, Interest, 4.0, Budget, Selection, 1));
	if (!TestEqual(TEXT("One new entity enters"), Selection.Records.Num(), 1))
		return false;
	TestTrue(TEXT("Selected record is an entry"), Selection.Records[0].Transition == EFlecsRelevanceTransition::Enter);
	return true;
}

#endif
