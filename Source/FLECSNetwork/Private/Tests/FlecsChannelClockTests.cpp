#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "FlecsReplicationChannel.h"
#include "Misc/AutomationTest.h"

/** Test-only friend: establish a validated server-side handshake and simulate a window ACK. */
struct FFlecsReplicationChannelTestAccessor
{
	static void ValidateServer(AFlecsReplicationChannel& Channel, uint64 Epoch)
	{
		Channel.bServerManifestValidated = true;
		Channel.HandshakeServerWorldEpoch = Epoch;
	}
	static void Acknowledge(AFlecsReplicationChannel& Channel, bool bStaleRevision)
	{
		const auto& Transfer = Channel.OutgoingSnapshotTransfer;
		Channel.HandleSnapshotWindowAcknowledgement(Transfer.WorldEpoch, Transfer.TransferId,
			Transfer.Revision + (bStaleRevision ? 1 : 0), Transfer.AwaitingThroughRecordIndex, true);
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsChannelClockTest, "Flecs.Network.Channel.HeartbeatStateOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsChannelClockTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Create transient test world"), World)) return false;
	AFlecsReplicationChannel* Channel = World->SpawnActor<AFlecsReplicationChannel>();
	if (!TestNotNull(TEXT("Spawn authority channel"), Channel)) { World->DestroyWorld(false); return false; }
	FFlecsReplicationChannelTestAccessor::ValidateServer(*Channel, 9);
	FFlecsReplicationRecord Record;
	Record.WorldEpoch = 9;
	Record.NetworkEntityId = 1;
	Record.EntityGeneration = 1;
	Record.DefinitionWireId = 1;
	Record.SourceSimulationTick = 100;
	Record.Operation = 3; // Entity create.
	TArray<FFlecsReplicationRecord> Batch = { Record };
	TestTrue(TEXT("Heartbeat tick 101 publishes"), Channel->PublishServerTickHeartbeat(9, 101));
	TestTrue(TEXT("Earlier state tick 100 is accepted after heartbeat"), Channel->PublishUpdateBatch(9, Batch, 100));
	TestTrue(TEXT("State waits for application ACK"), Channel->IsUpdateBatchTransferActive());
	FFlecsReplicationChannelTestAccessor::Acknowledge(*Channel, true);
	TestTrue(TEXT("Wrong revision cannot complete transfer"), Channel->IsUpdateBatchTransferActive());
	FFlecsReplicationChannelTestAccessor::Acknowledge(*Channel, false);
	TestFalse(TEXT("Matching ACK completes transfer"), Channel->IsUpdateBatchTransferActive());
	Batch[0].SourceSimulationTick = 99;
	TestFalse(TEXT("State tick older than previous state rejected"), Channel->PublishUpdateBatch(9, Batch, 99));
	Batch[0].SourceSimulationTick = 102;
	TestFalse(TEXT("Old epoch rejected"), Channel->PublishUpdateBatch(8, Batch, 102));
	TestFalse(TEXT("Heartbeat cannot regress"), Channel->PublishServerTickHeartbeat(9, 100));
	TestTrue(TEXT("Later state tick remains admissible"), Channel->PublishUpdateBatch(9, Batch, 102));
	FFlecsReplicationChannelTestAccessor::Acknowledge(*Channel, false);
	World->DestroyWorld(false);
	return true;
}

#endif
