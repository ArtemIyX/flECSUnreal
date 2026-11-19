#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Networking/FlecsNetworkLimits.h"
#include "Networking/FlecsSessionCallbacks.h"
#include "Settings/FlecsDeveloperSettings.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsNetworkLimitsTest, "Flecs.Network.Settings.CapacityAndBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsNetworkLimitsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FFlecsNetworkLimits Defaults;
	TestEqual(TEXT("Default world capacity"), Defaults.MaxEntities, 5000);
	TestEqual(TEXT("Default peer capacity"), Defaults.MaxRelevantEntitiesPerPeer, 5000);
	TestEqual(TEXT("Default owned capacity"), Defaults.MaxOwnedEntitiesPerPeer, 256);
	TestEqual(TEXT("Default selection record count"), Defaults.MaxRecordsPerSelection, 128u);
	TestEqual(TEXT("Default payload bytes"), Defaults.MaxPayloadBytesPerSelection, 65536u);
	UFlecsDeveloperSettings* Settings = NewObject<UFlecsDeveloperSettings>();
	Settings->MaxNetworkEntities = 100000;
	Settings->MaxPeerOwnedEntities = 100000;
	Settings->MaxRelevantEntitiesPerPeer = 100000;
	Settings->MaxRecordsPerPeerSelection = 16384;
	Settings->MaxPayloadKiBPerPeerSelection = 4096;
	FFlecsNetworkLimits Captured;
	TestTrue(TEXT("Maximum supported values are accepted"), FFlecsNetworkLimits::Capture(*Settings, Captured));
	TestEqual(TEXT("Maximum world capacity retained"), Captured.MaxEntities, 100000);
	TestEqual(TEXT("Maximum payload converted to bytes"), Captured.MaxPayloadBytesPerSelection, 4194304u);
	FFlecsRelevanceBudget PeerBudget = Captured.MakeSelectionBudget();
	FFlecsConnectionInterest Interest;
	FFlecsPeerSpawnContext Context;
	FString Error;
	bool bSawConfiguredBudget = false;
	FFlecsPeerInterestProvider Provider = [&bSawConfiguredBudget](const FFlecsPeerSpawnContext&,
		FFlecsConnectionInterest&, FFlecsRelevanceBudget& InOutBudget, FString&) {
		bSawConfiguredBudget = InOutBudget.MaxRecords == 16384 && InOutBudget.MaxBytes == 4194304;
		InOutBudget.MaxRecords = 20;
		InOutBudget.MaxBytes = 1024;
		return true;
	};
	TestTrue(TEXT("Per-peer provider accepts configured defaults"), Provider(Context, Interest, PeerBudget, Error));
	TestTrue(TEXT("Provider receives the captured budget"), bSawConfiguredBudget);
	TestEqual(TEXT("Provider can override records"), PeerBudget.MaxRecords, 20u);
	TestEqual(TEXT("Provider can override bytes"), PeerBudget.MaxBytes, 1024u);
	TestEqual(TEXT("Override does not mutate session defaults"), Captured.MaxRecordsPerSelection, 16384u);
	Settings->MaxNetworkEntities = 10;
	Settings->MaxPeerOwnedEntities = 99;
	Settings->MaxRelevantEntitiesPerPeer = 99;
	Settings->MaxRecordsPerPeerSelection = -1;
	Settings->MaxPayloadKiBPerPeerSelection = 5000;
	TestFalse(TEXT("Invalid limits report clamping"), FFlecsNetworkLimits::Capture(*Settings, Captured));
	TestEqual(TEXT("Owned capacity bounded by world"), Captured.MaxOwnedEntitiesPerPeer, 10);
	TestEqual(TEXT("Relevant capacity bounded by world"), Captured.MaxRelevantEntitiesPerPeer, 10);
	TestEqual(TEXT("Record count has positive lower bound"), Captured.MaxRecordsPerSelection, 1u);
	TestEqual(TEXT("Payload uses transport ceiling"), Captured.MaxPayloadBytesPerSelection, 4194304u);
	Settings->MaxNetworkEntities = 0;
	TestFalse(TEXT("Zero world capacity is rejected"), FFlecsNetworkLimits::Capture(*Settings, Captured));
	TestEqual(TEXT("World capacity clamps to one"), Captured.MaxEntities, 1);
	return true;
}

#endif
