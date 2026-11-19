#if WITH_DEV_AUTOMATION_TESTS
#include "Diagnostics/FlecsNetworkDiagnosticsSubsystem.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsDiagnosticsIsolationTest, "Flecs.Diagnostics.WorldIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlecsDiagnosticsIsolationTest::RunTest(const FString& Parameters)
{
 (void)Parameters;
 UWorld* FirstWorld = NewObject<UWorld>();
 UWorld* SecondWorld = NewObject<UWorld>();
 auto* First = NewObject<UFlecsNetworkDiagnosticsSubsystem>(FirstWorld);
 auto* Second = NewObject<UFlecsNetworkDiagnosticsSubsystem>(SecondWorld);
 if (!TestNotNull(TEXT("First world counters"), First) || !TestNotNull(TEXT("Second world counters"), Second)) return false;
 First->RecordPayload(0, true, 100);
 First->RecordPayload(1, false, 17);
 First->AddRelevance(3, 2, 1);
 TestEqual(TEXT("First world sent bytes"), First->GetSnapshot().Sent[0], uint64(100));
 TestEqual(TEXT("First world received bytes"), First->GetSnapshot().Received[1], uint64(17));
 TestEqual(TEXT("Second world remains independent"), Second->GetSnapshot().Sent[0], uint64(0));
 First->ResetRelevance();
 TestEqual(TEXT("Relevance reset is per world"), First->GetSnapshot().Relevant, uint32(0));
 TestEqual(TEXT("Reset does not discard payload totals"), First->GetSnapshot().Sent[0], uint64(100));
 return true;
}
#endif
