#if WITH_DEV_AUTOMATION_TESTS
#include "Diagnostics/FlecsLogicalStateBatch.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsLogicalStateBatchTest, "Flecs.Diagnostics.LogicalStateRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlecsLogicalStateBatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsLogicalStateBatch Tracker;
	FFlecsReplicationRecord R;
	R.WorldEpoch = 1;
	R.NetworkEntityId = 1;
	R.EntityGeneration = 1;
	R.SourceSimulationTick = 2;
	TArray<FFlecsReplicationRecord> Records = {R};
	TestTrue(TEXT("First logical batch counted"), Tracker.Accept(1, 2, Records));
	Records[0].SnapshotRevision = 99;
	Records[0].SnapshotTransferId = 5;
	TestFalse(TEXT("Transport retry ignored"), Tracker.Accept(1, 2, Records));
	Records[0].SourceSimulationTick = 3;
	TestTrue(TEXT("Changed state counted"), Tracker.Accept(1, 3, Records));
	const uint8 ChangedPayload[] = {7};
	Records[0].Payload.SetBytes(ChangedPayload, 1);
	TestTrue(TEXT("Different payload in same tick counted"), Tracker.Accept(1, 3, Records));
	TestFalse(TEXT("Duplicate changed payload ignored"), Tracker.Accept(1, 3, Records));
	TestTrue(TEXT("New epoch counted"), Tracker.Accept(2, 3, Records));
	return true;
}
#endif
