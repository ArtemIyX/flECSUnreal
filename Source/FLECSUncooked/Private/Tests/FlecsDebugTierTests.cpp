#if WITH_DEV_AUTOMATION_TESTS
#include "Diagnostics/FlecsDebugTier.h"
#include "Misc/AutomationTest.h"
#include <limits>
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsDebugTierTest, "Flecs.Diagnostics.EstimatedTier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFlecsDebugTierTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TArray<FFlecsDistanceUpdateRate> Tiers = {{100.0f, 30.0f, FColor::Green}, {200.0f, 10.0f, FColor::Red}};
	TestEqual(TEXT("Boundary in first tier"), FlecsDiagnostics::FindEstimatedTier(Tiers, 100.0), 0);
	TestEqual(TEXT("Outside first tier"), FlecsDiagnostics::FindEstimatedTier(Tiers, 100.1), 1);
	TestEqual(TEXT("Out of range"), FlecsDiagnostics::FindEstimatedTier(Tiers, 201.0), INDEX_NONE);
	TestEqual(TEXT("Missing position"), FlecsDiagnostics::FindEstimatedTier(Tiers, -1.0), INDEX_NONE);
	TestEqual(TEXT("Non-finite distance"),
		FlecsDiagnostics::FindEstimatedTier(Tiers, std::numeric_limits<double>::infinity()), INDEX_NONE);
	Tiers[1].MaxDistance = 100.0f;
	TestEqual(TEXT("Invalid settings rejected"), FlecsDiagnostics::FindEstimatedTier(Tiers, 20.0), INDEX_NONE);
	return true;
}
#endif
