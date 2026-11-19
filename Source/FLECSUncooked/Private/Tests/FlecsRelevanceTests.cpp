#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Relevance/FlecsRelevanceScheduler.h"

namespace
{
FFlecsSpatialEntity Entity(uint32 Id, float X, uint64 Owner = 0, bool bPrivate = false)
{
	FFlecsSpatialEntity Value;
	Value.Key = {1, Id, 1};
	Value.Position = FVector3f(X, 0, 0);
	Value.Revision = 1;
	Value.OwnerConnectionId = Owner;
	Value.EstimatedUpdateBytes = 32;
	Value.Visibility = bPrivate ? EFlecsNetworkVisibility::OwnerOnly : EFlecsNetworkVisibility::RelevantToAll;
	return Value;
}

FFlecsConnectionInterest Interest(uint64 Connection, float X)
{
	FFlecsConnectionInterest Value;
	Value.ConnectionId = Connection;
	Value.ViewPosition = FVector3f(X, 0, 0);
	Value.Tiers = {{100.0f, 10.0f}};
	Value.HysteresisDistance = 20.0f;
	return Value;
}

bool Has(const FFlecsRelevanceSelection& Selection, uint32 Id, EFlecsRelevanceTransition Transition)
{
	return Selection.Records.ContainsByPredicate([Id, Transition](const FFlecsScheduledEntity& Record) {
		return Record.Key.EntityId == Id && Record.Transition == Transition;
	});
}

bool Accept(FFlecsPerConnectionScheduler& Scheduler, const FFlecsRelevanceSelection& Selection, double Now)
{
	return Selection.SelectionId == 0 || Scheduler.CompleteSelection(Selection.SelectionId, true, Now);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceClientsTest, "Flecs.Network.Relevance.ClientsAndHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceClientsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.Entities = {Entity(1, 0), Entity(2, 1000), Entity(3, 0, 1, true)};
	FFlecsSpatialIndex Index;
	if (!TestTrue(TEXT("Build initial spatial index"), Index.Build(Snapshot, 100.0f)))
		return false;
	FFlecsPerConnectionScheduler First;
	FFlecsPerConnectionScheduler Second;
	FFlecsConnectionInterest A = Interest(1, 0);
	FFlecsConnectionInterest B = Interest(2, 1000);
	// An explicit visibility override must not reveal owner-only state.
	B.VisibilityOverrides.Add(Snapshot.Entities[2].Key, true);
	FFlecsRelevanceBudget Budget;
	FFlecsRelevanceSelection SelectionA, SelectionB;
	TestTrue(TEXT("Select first client"), First.Select(Index, A, 1.0, Budget, SelectionA));
	TestTrue(TEXT("Select second client"), Second.Select(Index, B, 1.0, Budget, SelectionB));
	TestTrue(TEXT("First client sees nearby public entity"), Has(SelectionA, 1, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Owner sees private entity"), Has(SelectionA, 3, EFlecsRelevanceTransition::Enter));
	TestFalse(TEXT("First client does not see distant entity"), Has(SelectionA, 2, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Second client sees its nearby entity"), Has(SelectionB, 2, EFlecsRelevanceTransition::Enter));
	TestFalse(
		TEXT("Override cannot reveal another owner's entity"), Has(SelectionB, 3, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Accept first handoff"), Accept(First, SelectionA, 1.0));
	TestTrue(TEXT("Accept second handoff"), Accept(Second, SelectionB, 1.0));

	Snapshot.Entities[0].Position.X = 110;
	Snapshot.Entities[0].Revision++;
	Snapshot.SimulationTick++;
	Index.Build(Snapshot, 100.0f);
	TestTrue(TEXT("Select within hysteresis"), First.Select(Index, A, 1.05, Budget, SelectionA));
	TestEqual(TEXT("Changed state is not sent before tier deadline"), SelectionA.Records.Num(), 0);
	TestTrue(TEXT("Select at tier deadline"), First.Select(Index, A, 1.11, Budget, SelectionA));
	TestTrue(TEXT("Previously relevant entity stays inside hysteresis"),
		Has(SelectionA, 1, EFlecsRelevanceTransition::Update));
	TestTrue(TEXT("Accept due update"), Accept(First, SelectionA, 1.11));
	Snapshot.Entities[0].Position.X = 121;
	Snapshot.SimulationTick++;
	Index.Build(Snapshot, 100.0f);
	TestTrue(TEXT("Select beyond hysteresis"), First.Select(Index, A, 1.12, Budget, SelectionA));
	TestTrue(TEXT("Leaving hysteresis emits exit immediately"), Has(SelectionA, 1, EFlecsRelevanceTransition::Exit));
	TestTrue(TEXT("Accept exit"), Accept(First, SelectionA, 1.12));
	Snapshot.Entities[2].OwnerConnectionId = 2;
	Snapshot.SimulationTick++;
	Index.Build(Snapshot, 100.0f);
	TestTrue(TEXT("Select after ownership changes"), First.Select(Index, A, 1.13, Budget, SelectionA));
	TestTrue(TEXT("Old owner receives private-state exit"), Has(SelectionA, 3, EFlecsRelevanceTransition::Exit));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceBudgetTest, "Flecs.Network.Relevance.BudgetAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceBudgetTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.Entities = {Entity(1, 0), Entity(2, 0), Entity(3, 0)};
	FFlecsSpatialIndex Index;
	if (!TestTrue(TEXT("Build index"), Index.Build(Snapshot, 100)))
		return false;
	FFlecsPerConnectionScheduler Scheduler;
	const FFlecsConnectionInterest Client = Interest(1, 0);
	FFlecsRelevanceBudget Budget;
	Budget.MaxRecords = 1;
	Budget.MaxBytes = 32;
	FFlecsRelevanceSelection Selection;
	TestTrue(TEXT("First selection"), Scheduler.Select(Index, Client, 1.0, Budget, Selection));
	if (!TestEqual(TEXT("One record fits budget"), Selection.Records.Num(), 1))
		return false;
	const uint32 FirstId = Selection.Records[0].Key.EntityId;
	TestEqual(TEXT("Remaining records deferred"), Selection.Metrics.DeferredByBudget, uint32(2));
	TestFalse(TEXT("Stale handoff token rejected"), Scheduler.CompleteSelection(Selection.SelectionId + 1, true, 1.0));
	TestTrue(
		TEXT("Rejected handoff clears assignment"), Scheduler.CompleteSelection(Selection.SelectionId, false, 1.0));
	TSet<uint32> Accepted;
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		const double Now = 1.01 + 0.01 * Attempt;
		TestTrue(TEXT("Select deferred entity"), Scheduler.Select(Index, Client, Now, Budget, Selection));
		if (!TestEqual(TEXT("Exactly one deferred entity selected"), Selection.Records.Num(), 1))
			return false;
		Accepted.Add(Selection.Records[0].Key.EntityId);
		TestTrue(TEXT("Accept selection"), Accept(Scheduler, Selection, Now));
	}
	TestEqual(TEXT("All three entities eventually handed off"), Accepted.Num(), 3);
	TestTrue(TEXT("Rejected entity was retried"), Accepted.Contains(FirstId));
	Budget.MaxBytes = 31;
	Snapshot.Entities[0].Revision++;
	Index.Build(Snapshot, 100);
	TestTrue(TEXT("Selection respects byte limit"), Scheduler.Select(Index, Client, 2.0, Budget, Selection));
	TestEqual(TEXT("Record larger than byte budget is deferred"), Selection.Records.Num(), 0);
	TestTrue(TEXT("Byte deferral is measured"), Selection.Metrics.DeferredByBudget > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceIdentityTest, "Flecs.Network.Relevance.StaleIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 2;
	Snapshot.SimulationTick = 1;
	FFlecsSpatialEntity Current = Entity(1, 0);
	Current.Key.WorldEpoch = 2;
	Current.Key.Generation = 2;
	Snapshot.Entities.Add(Current);
	FFlecsSpatialIndex Index;
	TestTrue(TEXT("Current epoch builds"), Index.Build(Snapshot, 100));
	FFlecsPerConnectionScheduler Scheduler;
	FFlecsRelevanceSelection Selection;
	const FFlecsConnectionInterest Client = Interest(1, 0);
	FFlecsRelevanceBudget Budget;
	TestTrue(TEXT("Current epoch selects"), Scheduler.Select(Index, Client, 1, Budget, Selection));
	TestTrue(TEXT("Current epoch handed off"), Accept(Scheduler, Selection, 1));
	Snapshot.WorldEpoch = 1;
	Snapshot.Entities[0].Key.WorldEpoch = 1;
	TestTrue(TEXT("Old epoch index builds independently"), Index.Build(Snapshot, 100));
	TestFalse(TEXT("Scheduler rejects old epoch publication"), Scheduler.Select(Index, Client, 2, Budget, Selection));
	Snapshot.WorldEpoch = 2;
	Snapshot.Entities[0] = Current;
	FFlecsSpatialEntity Stale = Current;
	Stale.Key.Generation = 1;
	Snapshot.Entities.Add(Stale);
	TestFalse(TEXT("Index rejects simultaneous generations of one network ID"), Index.Build(Snapshot, 100));
	Snapshot.Entities.Reset();
	Snapshot.SimulationTick++;
	TestTrue(TEXT("Despawn publication builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("Despawn selects"), Scheduler.Select(Index, Client, 3, Budget, Selection));
	TestTrue(
		TEXT("Old generation exits"), Selection.Records.ContainsByPredicate([](const FFlecsScheduledEntity& Record) {
			return Record.Key.EntityId == 1 && Record.Key.Generation == 2
				&& Record.Transition == EFlecsRelevanceTransition::Exit;
		}));
	TestTrue(TEXT("Despawn handed off"), Accept(Scheduler, Selection, 3));
	Current.Key.Generation = 3;
	Snapshot.Entities.Add(Current);
	Snapshot.SimulationTick++;
	TestTrue(TEXT("Reused ID with newer generation builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("New generation selects"), Scheduler.Select(Index, Client, 4, Budget, Selection));
	TestTrue(
		TEXT("New generation enters"), Selection.Records.ContainsByPredicate([](const FFlecsScheduledEntity& Record) {
			return Record.Key.EntityId == 1 && Record.Key.Generation == 3
				&& Record.Transition == EFlecsRelevanceTransition::Enter;
		}));
	TestTrue(TEXT("Reuse transitions handed off"), Accept(Scheduler, Selection, 4));
	Snapshot.Entities[0].Key.Generation = 2;
	Snapshot.SimulationTick++;
	TestTrue(TEXT("Stale generation index builds independently"), Index.Build(Snapshot, 100));
	TestFalse(TEXT("Scheduler rejects a regressed generation"), Scheduler.Select(Index, Client, 5, Budget, Selection));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsPartialSpatialIdentityTest, "Flecs.Network.Relevance.PartialSpatialIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsPartialSpatialIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.bHasCompleteLiveEntitySet = true;
	const FFlecsSpatialEntity Live = Entity(1, 0);
	Snapshot.Entities.Add(Live);
	Snapshot.LiveEntityKeys.Add(Live.Key);
	FFlecsSpatialIndex Index;
	if (!TestTrue(TEXT("Complete snapshot builds"), Index.Build(Snapshot, 100)))
		return false;
	FFlecsPerConnectionScheduler Scheduler;
	const FFlecsConnectionInterest Client = Interest(1, 0);
	FFlecsRelevanceBudget Budget;
	FFlecsRelevanceSelection Selection;
	TestTrue(TEXT("Initial entity selected"), Scheduler.Select(Index, Client, 1.0, Budget, Selection));
	TestTrue(TEXT("Initial entity enters"), Has(Selection, 1, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Initial enter handed off"), Accept(Scheduler, Selection, 1.0));

	// The identity remains live although its spatial extractor temporarily failed.
	Snapshot.Entities.Reset();
	Snapshot.SimulationTick = 2;
	TestTrue(TEXT("Partial spatial data with complete live set builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("Missing spatial metadata selects safely"), Scheduler.Select(Index, Client, 1.2, Budget, Selection));
	TestEqual(TEXT("Live identity with unavailable metadata is not exited"), Selection.Records.Num(), 0);
	Snapshot.Entities.Add(Live);
	Snapshot.Entities[0].Revision = 2;
	Snapshot.SimulationTick = 3;
	TestTrue(TEXT("Recovered metadata builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("Recovered same generation selects"), Scheduler.Select(Index, Client, 1.4, Budget, Selection));
	TestTrue(TEXT("Recovered entity updates without spurious re-entry"),
		Has(Selection, 1, EFlecsRelevanceTransition::Update));
	TestTrue(TEXT("Recovered update handed off"), Accept(Scheduler, Selection, 1.4));

	Snapshot.Entities.Reset();
	Snapshot.LiveEntityKeys.Reset();
	Snapshot.SimulationTick = 4;
	TestTrue(TEXT("Complete empty live set builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("Authoritative disappearance selects"), Scheduler.Select(Index, Client, 1.5, Budget, Selection));
	TestTrue(TEXT("Only authoritative absence emits despawn"),
		Selection.Records.ContainsByPredicate([](const FFlecsScheduledEntity& Record) {
			return Record.Key.EntityId == 1 && Record.Transition == EFlecsRelevanceTransition::Exit
				&& Record.bEntityDespawned;
		}));
	TestTrue(TEXT("Despawn handed off"), Accept(Scheduler, Selection, 1.5));
	Snapshot.Entities.Add(Live);
	Snapshot.LiveEntityKeys.Add(Live.Key);
	Snapshot.SimulationTick = 5;
	TestTrue(TEXT("Stale identity snapshot builds independently"), Index.Build(Snapshot, 100));
	TestFalse(
		TEXT("Retired generation cannot return as live"), Scheduler.Select(Index, Client, 1.6, Budget, Selection));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceCapacityTest, "Flecs.Network.Relevance.PerPeerCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceCapacityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.Entities = {Entity(1, 0), Entity(2, 0)};
	Snapshot.Entities[0].Importance = 100;
	FFlecsSpatialIndex Index;
	if (!TestTrue(TEXT("Build index"), Index.Build(Snapshot, 100)))
		return false;
	FFlecsPerConnectionScheduler Scheduler;
	FFlecsConnectionInterest Client = Interest(1, 0);
	FFlecsRelevanceBudget Budget;
	FFlecsRelevanceSelection Selection;
	TestTrue(TEXT("Select with one entity slot"), Scheduler.Select(Index, Client, 1.0, Budget, Selection, 1));
	TestTrue(TEXT("Higher priority entity enters"), Has(Selection, 1, EFlecsRelevanceTransition::Enter));
	TestEqual(TEXT("Second entity deferred by capacity"), Selection.Metrics.DeferredByCapacity, 1u);
	TestTrue(TEXT("Acknowledge initial admission"), Accept(Scheduler, Selection, 1.0));
	Client.VisibilityOverrides.Add(Snapshot.Entities[0].Key, false);
	TestTrue(TEXT("Select exit"), Scheduler.Select(Index, Client, 2.0, Budget, Selection, 1));
	TestTrue(TEXT("Previously admitted entity exits"), Has(Selection, 1, EFlecsRelevanceTransition::Exit));
	TestFalse(TEXT("Exit does not release capacity before acknowledgement"),
		Has(Selection, 2, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Acknowledge exit"), Accept(Scheduler, Selection, 2.0));
	TestTrue(TEXT("Select after exit"), Scheduler.Select(Index, Client, 3.0, Budget, Selection, 1));
	TestTrue(TEXT("Deferred entity enters after acknowledgement"), Has(Selection, 2, EFlecsRelevanceTransition::Enter));
	FFlecsPerConnectionScheduler BlockedScheduler;
	TMap<FFlecsNetworkEntityKey, double> Blocked;
	Blocked.Add(Snapshot.Entities[0].Key, 2.0);
	FFlecsConnectionInterest UnfilteredClient = Interest(1, 0);
	TestTrue(TEXT("Select while one entity is temporarily capacity-blocked"),
		BlockedScheduler.Select(Index, UnfilteredClient, 1.0, Budget, Selection, 1, &Blocked));
	TestFalse(TEXT("Blocked entity is deferred"), Has(Selection, 1, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Another entity can use the slot"), Has(Selection, 2, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Reject in-flight selection before retry"),
		BlockedScheduler.CompleteSelection(Selection.SelectionId, false, 1.0));
	TestTrue(TEXT("Select after the retry deadline"),
		BlockedScheduler.Select(Index, UnfilteredClient, 3.0, Budget, Selection, 1, &Blocked));
	TestTrue(TEXT("Previously blocked entity can be retried"), Has(Selection, 1, EFlecsRelevanceTransition::Enter));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsLargeWorldRelevanceTest, "Flecs.Network.Relevance.LargeWorldCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsLargeWorldRelevanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.Entities.Reserve(FFlecsSpatialIndex::MaxEntities);
	Snapshot.LiveEntityKeys.Reserve(FFlecsSpatialIndex::MaxEntities);
	for (int32 Id = 1; Id <= FFlecsSpatialIndex::MaxEntities; ++Id)
	{
		FFlecsSpatialEntity Value = Entity(static_cast<uint32>(Id), 0);
		if (Id <= 2)
			Value.Importance = 100;
		Snapshot.LiveEntityKeys.Add(Value.Key);
		Snapshot.Entities.Add(MoveTemp(Value));
	}
	Snapshot.bHasCompleteLiveEntitySet = true;
	FFlecsSpatialIndex Index;
	if (!TestTrue(TEXT("Build a 100,000-entity spatial index"), Index.Build(Snapshot, 100)))
		return false;
	FFlecsPerConnectionScheduler Scheduler;
	FFlecsRelevanceSelection Selection;
	FFlecsRelevanceBudget Budget;
	Budget.MaxRecords = 2;
	TestTrue(TEXT("Select from the large world"), Scheduler.Select(Index, Interest(1, 0), 1.0, Budget, Selection, 2));
	TestEqual(TEXT("Per-peer cap admits two entities"), Selection.Records.Num(), 2);
	TestTrue(TEXT("First high-priority entity admitted"), Has(Selection, 1, EFlecsRelevanceTransition::Enter));
	TestTrue(TEXT("Second high-priority entity admitted"), Has(Selection, 2, EFlecsRelevanceTransition::Enter));
	TestEqual(TEXT("Other entities deferred by capacity"), Selection.Metrics.DeferredByCapacity, 99998u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsRelevanceHistoryLimitTest, "Flecs.Network.Relevance.HistoryExhaustion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsRelevanceHistoryLimitTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FFlecsSpatialSnapshot Snapshot;
	Snapshot.WorldEpoch = 1;
	Snapshot.SimulationTick = 1;
	Snapshot.Entities = {Entity(1, 0), Entity(2, 0)};
	FFlecsSpatialIndex Index;
	FFlecsPerConnectionScheduler Scheduler(2);
	FFlecsRelevanceBudget Budget;
	FFlecsRelevanceSelection Selection;
	const FFlecsConnectionInterest Client = Interest(1, 0);
	TestTrue(TEXT("Initial index builds"), Index.Build(Snapshot, 100));
	FFlecsPerConnectionScheduler TooSmall(1);
	AddExpectedError(TEXT("Flecs relevance identity history limit reached"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("An oversized first snapshot is rejected before history changes"),
		TooSmall.Select(Index, Client, 1.0, Budget, Selection));
	TestTrue(TEXT("First-snapshot history exhaustion is reported"), Selection.Metrics.bIdentityHistoryExhausted);
	TestTrue(TEXT("Initial history fits"), Scheduler.Select(Index, Client, 1.0, Budget, Selection));
	TestTrue(TEXT("Acknowledge initial selection"), Accept(Scheduler, Selection, 1.0));
	Snapshot.Entities[1] = Entity(3, 0);
	++Snapshot.SimulationTick;
	TestTrue(TEXT("Over-history-limit index still builds"), Index.Build(Snapshot, 100));
	AddExpectedError(TEXT("Flecs relevance identity history limit reached"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("A new identity beyond history capacity is rejected"),
		Scheduler.Select(Index, Client, 2.0, Budget, Selection));
	TestTrue(TEXT("History exhaustion is reported to the session"), Selection.Metrics.bIdentityHistoryExhausted);
	Snapshot.Entities[1] = Entity(2, 0);
	++Snapshot.SimulationTick;
	TestTrue(TEXT("Known identities still build"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("Rejected snapshot did not corrupt previous history"),
		Scheduler.Select(Index, Client, 3.0, Budget, Selection));
	TestTrue(TEXT("Complete any known-identity selection"), Accept(Scheduler, Selection, 3.0));
	Snapshot.WorldEpoch = 2;
	Snapshot.Entities = {Entity(3, 0), Entity(4, 0)};
	for (FFlecsSpatialEntity& Item : Snapshot.Entities)
		Item.Key.WorldEpoch = 2;
	++Snapshot.SimulationTick;
	TestTrue(TEXT("New epoch index builds"), Index.Build(Snapshot, 100));
	TestTrue(TEXT("New epoch starts fresh bounded history"), Scheduler.Select(Index, Client, 4.0, Budget, Selection));
	return true;
}

#endif
