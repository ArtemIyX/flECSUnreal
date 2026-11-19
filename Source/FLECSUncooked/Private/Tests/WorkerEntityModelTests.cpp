#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "FlecsSimulationTypes.h"
#include "Misc/AutomationTest.h"
#include "Simulation/FlecsWorkerEntityModel.h"

using namespace FlecsModRegistry;
using namespace FlecsSimulation;

namespace
{
	struct FTestState
	{
		TMap<uint64, TArray<uint8>> PublicValues;
		TMap<uint64, TArray<uint8>> PrivateValues;
		uint32 Commands = 0;
		uint32 Removals = 0;
	};

	bool RegisterFixture(FRegistry& Registry, TSharedRef<FTestState> State)
	{
		FString Error;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const bool bPrivate = Index == 1;
			FComponentSchema Schema;
			Schema.StableId = bPrivate ? TEXT("test:private") : TEXT("test:public");
			Schema.MaxEncodedBytes = 4;
			Schema.Visibility = bPrivate ? EVisibilityPolicy::OwnerOnly : EVisibilityPolicy::Relevant;
			Schema.AllowedEntityDefinitions.Add(TEXT("test:entity"));
			Schema.Extract = [State, bPrivate](uint64 Token, TArray<uint8>& Out)
			{
				const TArray<uint8>* Value = (bPrivate ? State->PrivateValues : State->PublicValues).Find(Token);
				if (!Value) return false;
				Out = *Value;
				return true;
			};
			Schema.Apply = [State, bPrivate](uint64 Token, TConstArrayView<uint8> Bytes)
			{
				if (Bytes.Num() != 1) return false;
				TArray<uint8>& Value = (bPrivate ? State->PrivateValues : State->PublicValues).FindOrAdd(Token);
				Value.Reset();
				Value.Append(Bytes.GetData(), Bytes.Num());
				// Deliberately mutate before failure to exercise model rollback.
				return Bytes[0] != 255;
			};
			Schema.Remove = [State, bPrivate](uint64 Token)
			{
				(bPrivate ? State->PrivateValues : State->PublicValues).Remove(Token);
				++State->Removals;
			};
			if (!Registry.RegisterComponent(MoveTemp(Schema), Error)) return false;
		}
		FEntityDefinition Definition;
		Definition.StableId = TEXT("test:entity");
		Definition.ComponentIds = { TEXT("test:public"), TEXT("test:private") };
		Definition.MaxInitialStateBytes = 8;
		Definition.Create = [](uint64 Token) { return Token != 0; };
		if (!Registry.RegisterEntityDefinition(MoveTemp(Definition), Error)) return false;
		FEntityDefinition Alternate;
		Alternate.StableId = TEXT("test:alternate");
		Alternate.ComponentIds = { TEXT("test:public"), TEXT("test:private") };
		Alternate.MaxInitialStateBytes = 8;
		Alternate.Create = [](uint64 Token) { return Token != 0; };
		if (!Registry.RegisterEntityDefinition(MoveTemp(Alternate), Error)) return false;
		FCommandSchema Command;
		Command.StableId = TEXT("test:command");
		Command.MaxPayloadBytes = 1;
		Command.RequiredPermission = TEXT("test:permission");
		Command.Execute = [State](uint64, uint64 Token, TConstArrayView<uint8> Payload)
		{
			if (Token == 0 || Payload.Num() != 1 || Payload[0] != 42) return false;
			++State->Commands;
			return true;
		};
		return Registry.RegisterCommand(MoveTemp(Command), Error) && Registry.Freeze(1, Error);
	}

	FStateRecord Record(uint32 Id, uint16 Generation, uint16 Definition, uint16 Component,
		EStateOperation Operation, uint32 Tick, uint8 Value = 0)
	{
		FStateRecord Item;
		Item.Identity.ServerWorldEpoch = 9;
		Item.Identity.NetworkEntityId = Id;
		Item.Identity.Generation = Generation;
		Item.DefinitionWireId = Definition;
		Item.ComponentWireId = Component;
		Item.Operation = Operation;
		Item.SourceSimulationTick = Tick;
		if (Operation == EStateOperation::ComponentUpsert) Item.Payload.Add(Value);
		return Item;
	}

	void Expect(TArray<FString>& Failures, bool bCondition, const TCHAR* Message)
	{
		if (!bCondition) Failures.Add(Message);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorkerEntityModelTest, "Flecs.Simulation.EntityModelContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorkerEntityModelTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TFuture<TArray<FString>> Future = Async(EAsyncExecution::Thread, []()
	{
		TArray<FString> Failures;
		auto ServerState = MakeShared<FTestState>();
		auto ClientState = MakeShared<FTestState>();
		FRegistry ServerRegistry, ClientRegistry;
		if (!RegisterFixture(ServerRegistry, ServerState) || !RegisterFixture(ClientRegistry, ClientState))
		{
			Failures.Add(TEXT("Fixture registration failed"));
			return Failures;
		}
		const FCompatibilityResult Compatibility = ServerRegistry.CompareRequired(ClientRegistry);
		if (!Compatibility.bCompatible) { Failures.Add(TEXT("Fixture schemas differ")); return Failures; }
		const FSessionMapping& Mapping = Compatibility.SessionMapping;
		uint16 DefinitionWire = 0, PublicWire = 0, PrivateWire = 0, CommandWire = 0;
		if (!Mapping.FindWireId(EEntryKind::EntityDefinition, TEXT("test:entity"), DefinitionWire) ||
			!Mapping.FindWireId(EEntryKind::Component, TEXT("test:public"), PublicWire) ||
			!Mapping.FindWireId(EEntryKind::Component, TEXT("test:private"), PrivateWire) ||
			!Mapping.FindWireId(EEntryKind::Command, TEXT("test:command"), CommandWire))
		{
			Failures.Add(TEXT("Missing session wire mapping")); return Failures;
		}
		flecs::world ServerWorld;
		flecs::world ClientWorld;
		FWorkerEntityModel Server(ServerRegistry, true);
		FWorkerEntityModel Client(ClientRegistry, false);
		Expect(Failures, Server.Initialize(ServerWorld, 9, [](uint64 Connection, const FString& Permission, uint64 Owner, uint32, uint16)
			{ return Connection == Owner && Permission == TEXT("test:permission"); }) == EModelResult::Accepted, TEXT("Server initialize"));
		Expect(Failures, Client.Initialize(ClientWorld, 9) == EModelResult::Accepted, TEXT("Client initialize"));
		TArray<FInitialComponent> Initial = { { TEXT("test:public"), { 7 } }, { TEXT("test:private"), { 8 } } };
		FNetworkEntityIdentity Identity;
		if (Server.SpawnAuthoritative(TEXT("test:entity"), 10, Initial, Identity) != EModelResult::Accepted)
		{
			Failures.Add(TEXT("Authoritative spawn failed")); Server.Shutdown(); Client.Shutdown(); return Failures;
		}
		FPeerReplicationContext Owner, Other;
		Owner.ConnectionToken = 10;
		Other.ConnectionToken = 11;
		Owner.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Identity.NetworkEntityId, Identity.Generation));
		Other.RelevantEntityKeys = Owner.RelevantEntityKeys;
		FStateBatch OwnerBatch, OtherBatch;
		Expect(Failures, Server.BuildAuthoritativeSnapshot(10, Owner, Mapping, OwnerBatch) == EModelResult::Accepted, TEXT("Owner snapshot"));
		Expect(Failures, Server.BuildAuthoritativeSnapshot(10, Other, Mapping, OtherBatch) == EModelResult::Accepted, TEXT("Other snapshot"));
		Expect(Failures, OwnerBatch.Records.ContainsByPredicate([&](const FStateRecord& R) { return R.ComponentWireId == PrivateWire; }), TEXT("Owner receives private component"));
		Expect(Failures, !OtherBatch.Records.ContainsByPredicate([&](const FStateRecord& R) { return R.ComponentWireId == PrivateWire; }), TEXT("Other peer cannot receive private component"));

		FStateBatch Mixed;
		Mixed.ServerWorldEpoch = 9;
		Mixed.SimulationTick = 12;
		Mixed.bCompleteStateSnapshot = true;
		Mixed.Records = { Record(Identity.NetworkEntityId, Identity.Generation, DefinitionWire, 0, EStateOperation::EntityCreate, 12),
			Record(Identity.NetworkEntityId, Identity.Generation, DefinitionWire, PublicWire, EStateOperation::ComponentUpsert, 10, 7),
			Record(Identity.NetworkEntityId, Identity.Generation, DefinitionWire, PrivateWire, EStateOperation::ComponentUpsert, 12, 8) };
		Expect(Failures, Client.ApplyClientBatch(Mixed, Mapping) == EModelResult::Accepted, TEXT("Initial mixed-age snapshot"));
		Expect(Failures, Client.ApplyClientBatch(Mixed, Mapping) == EModelResult::Accepted, TEXT("Repeated mixed-age snapshot is idempotent"));
		FStateBatch Stale = Mixed;
		Stale.ServerWorldEpoch = 8;
		Expect(Failures, Client.ApplyClientBatch(Stale, Mapping) == EModelResult::WrongEpoch, TEXT("Old epoch rejected"));
		Stale = Mixed;
		Stale.SimulationTick = 11;
		Expect(Failures, Client.ApplyClientBatch(Stale, Mapping) == EModelResult::StaleTick, TEXT("Old batch tick rejected"));
		FStateBatch Exit;
		Exit.ServerWorldEpoch = 9;
		Exit.SimulationTick = 13;
		Exit.bCompleteStateSnapshot = true;
		Expect(Failures, Client.ApplyClientBatch(Exit, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 0 &&
			ClientState->PublicValues.IsEmpty() && ClientState->PrivateValues.IsEmpty(), TEXT("Relevance exit cleans state"));
		Mixed.SimulationTick = 14;
		Mixed.Records[0].SourceSimulationTick = 14;
		Expect(Failures, Client.ApplyClientBatch(Mixed, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 1,
			TEXT("Same-generation relevance re-entry succeeds"));
		FStateBatch Removal;
		Removal.ServerWorldEpoch = 9;
		Removal.SimulationTick = 15;
		Removal.Records = { Record(Identity.NetworkEntityId, Identity.Generation, DefinitionWire, PrivateWire, EStateOperation::ComponentRemove, 15) };
		Expect(Failures, Client.ApplyClientBatch(Removal, Mapping) == EModelResult::Accepted && ClientState->PrivateValues.IsEmpty(), TEXT("Component removal cleans state"));
		FStateBatch Despawn;
		Despawn.ServerWorldEpoch = 9;
		Despawn.SimulationTick = 16;
		Despawn.Records = { Record(Identity.NetworkEntityId, Identity.Generation, DefinitionWire, 0, EStateOperation::EntityDespawn, 16) };
		Expect(Failures, Client.ApplyClientBatch(Despawn, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 0 && ClientState->PublicValues.IsEmpty(), TEXT("Despawn cleans remaining state"));
		Mixed.SimulationTick = 17;
		Mixed.Records[0].SourceSimulationTick = 17;
		Expect(Failures, Client.ApplyClientBatch(Mixed, Mapping) != EModelResult::Accepted, TEXT("Despawned generation rejected"));

		FClientCommand Cmd;
		Cmd.ServerWorldEpoch = 9;
		Cmd.TrustedConnectionToken = 11;
		Cmd.Sequence = 1;
		Cmd.TargetNetworkEntityId = Identity.NetworkEntityId;
		Cmd.TargetGeneration = Identity.Generation;
		Cmd.RequestedSimulationTick = 100;
		Cmd.CommandWireId = CommandWire;
		Cmd.Payload = { 42 };
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::Unauthorized, TEXT("Non-owner command rejected"));
		Cmd.TrustedConnectionToken = 10;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::Accepted, TEXT("Authorized command executes"));
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::Replay, TEXT("Duplicate reliable command rejected"));
		Cmd.Sequence = 2;
		Cmd.bDiscrete = false;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::Accepted, TEXT("Continuous sequence is independent"));
		Cmd.ServerWorldEpoch = 8;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::WrongEpoch, TEXT("Command epoch rejected"));
		Cmd.ServerWorldEpoch = 9;
		Cmd.TargetGeneration++;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::StaleIdentity, TEXT("Command generation rejected"));
		Cmd.TargetGeneration = Identity.Generation;
		Cmd.RequestedSimulationTick = 100 + MaxFutureCommandTicks + 1;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::StaleTick, TEXT("Future command rejected"));
		Cmd.RequestedSimulationTick = 100;
		Cmd.Payload = { 42, 42 };
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) != EModelResult::Accepted, TEXT("Oversize command rejected"));
		Expect(Failures, ServerState->Commands == 2, TEXT("Rejected commands did not execute"));
		Cmd.Payload = { 42 };
		bool bAcceptedWithinBudget = true;
		for (uint32 Sequence = 3; Sequence <= 60; ++Sequence)
		{
			Cmd.Sequence = Sequence;
			bAcceptedWithinBudget &= Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::Accepted;
		}
		Expect(Failures, bAcceptedWithinBudget, TEXT("Commands within rate budget execute"));
		Cmd.Sequence = 61;
		Expect(Failures, Server.ExecuteAuthoritativeCommand(Cmd, 100, Mapping) == EModelResult::RateLimited, TEXT("Command rate limit enforced"));
		Expect(Failures, ServerState->Commands == 60, TEXT("Rate-limited command did not execute"));
		Server.Shutdown();
		Client.Shutdown();
		return Failures;
	});
	for (const FString& Failure : Future.Get()) AddError(Failure);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorkerDeltaAndSpatialTest, "Flecs.Simulation.DeltaAndSpatialContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorkerDeltaAndSpatialTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TFuture<TArray<FString>> Future = Async(EAsyncExecution::Thread, []()
	{
		TArray<FString> Failures;
		auto State = MakeShared<FTestState>();
		FRegistry Registry;
		if (!RegisterFixture(Registry, State)) { Failures.Add(TEXT("Delta fixture registration failed")); return Failures; }
		const FCompatibilityResult Compatibility = Registry.CompareRequired(Registry);
		if (!Compatibility.bCompatible) { Failures.Add(TEXT("Delta fixture compatibility failed")); return Failures; }
		const FSessionMapping& Mapping = Compatibility.SessionMapping;
		uint16 PublicWire = 0, PrivateWire = 0;
		Mapping.FindWireId(EEntryKind::Component, TEXT("test:public"), PublicWire);
		Mapping.FindWireId(EEntryKind::Component, TEXT("test:private"), PrivateWire);
		flecs::world World;
		FWorkerEntityModel Model(Registry, true);
		Expect(Failures, Model.Initialize(World, 9, [](uint64, const FString&, uint64, uint32, uint16) { return true; }) == EModelResult::Accepted, TEXT("Delta model initializes"));
		Expect(Failures, Model.RegisterSpatialDefinition(TEXT("test:entity"), ESpatialDefinitionPolicy::PositionRequired,
			[State](uint64 Token, FVector3d& Out)
			{
				const TArray<uint8>* Value = State->PublicValues.Find(Token);
				if (!Value || Value->IsEmpty() || (*Value)[0] == 3) return false;
				Out = FVector3d((*Value)[0], 0, 0);
				return true;
			}) == EModelResult::Accepted, TEXT("Spatial definition registration succeeds"));
		TArray<FInitialComponent> Initial = { { TEXT("test:public"), { 7 } }, { TEXT("test:private"), { 8 } } };
		FNetworkEntityIdentity Identity;
		if (Model.SpawnAuthoritative(TEXT("test:entity"), 10, Initial, Identity) != EModelResult::Accepted)
		{
			Failures.Add(TEXT("Delta model spawn failed")); Model.Shutdown(); return Failures;
		}
		Expect(Failures, Model.RegisterSpatialDefinition(TEXT("test:entity"), ESpatialDefinitionPolicy::SkipWithoutPosition) != EModelResult::Accepted,
			TEXT("Definition policy cannot change after spawn"));
		TArray<FSpatialEntityMetadata> Metadata;
		TArray<FSpatialMetadataDiagnostic> Diagnostics;
		Expect(Failures, Model.BuildSpatialMetadata(1, Metadata, Diagnostics) == EModelResult::Accepted &&
			Metadata.Num() == 1 && Metadata[0].Position.X == 7 && Diagnostics.IsEmpty(), TEXT("Valid spatial accessor produces metadata"));
		Initial[0].Payload = { 3 };
		FNetworkEntityIdentity Broken;
		Expect(Failures, Model.SpawnAuthoritative(TEXT("test:entity"), 11, Initial, Broken) == EModelResult::Accepted, TEXT("Second entity spawns"));
		Expect(Failures, Model.BuildSpatialMetadata(2, Metadata, Diagnostics) == EModelResult::Partial &&
			Metadata.Num() == 1 && Diagnostics.Num() == 1 && Diagnostics[0].Code == ESpatialMetadataDiagnosticCode::PositionAccessorFailed,
			TEXT("Bad accessor is isolated with partial diagnostic"));
		TArray<FNetworkEntityIdentity> LiveIdentities;
		Expect(Failures, Model.BuildLiveEntityIdentities(LiveIdentities) == EModelResult::Accepted && LiveIdentities.Num() == 2 &&
			LiveIdentities.ContainsByPredicate([&](const FNetworkEntityIdentity& Item) { return Item.NetworkEntityId == Identity.NetworkEntityId && Item.Generation == Identity.Generation; }) &&
			LiveIdentities.ContainsByPredicate([&](const FNetworkEntityIdentity& Item) { return Item.NetworkEntityId == Broken.NetworkEntityId && Item.Generation == Broken.Generation; }),
			TEXT("Live identity enumeration includes entities omitted from spatial metadata"));
		FWorkerEntityModel Uninitialized(Registry, true);
		Expect(Failures, Uninitialized.BuildLiveEntityIdentities(LiveIdentities) == EModelResult::WrongRole && LiveIdentities.IsEmpty(),
			TEXT("Failed live identity enumeration clears output"));

		FPeerReplicationContext Peer;
		Peer.ConnectionToken = 10;
		Peer.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Identity.NetworkEntityId, Identity.Generation));
		FSelectedEntityComponents Selected;
		Selected.Identity = Identity;
		TArray<FSelectedEntityComponents> Due = { Selected };
		FStateBatch Batch, Retry;
		Expect(Failures, Model.BuildAuthoritativeDelta(10, Peer, Mapping, Due, Batch) == EModelResult::Accepted && Batch.PublicationRevision != 0 && Batch.Records.Num() == 3,
			TEXT("Initial delta contains full visible state"));
		const uint64 FirstRevision = Batch.PublicationRevision;
		Expect(Failures, Model.BuildAuthoritativeDelta(11, Peer, Mapping, Due, Retry) == EModelResult::RateLimited, TEXT("Only one delta may be pending per peer"));
		Expect(Failures, Model.AcknowledgeAuthoritativeDelta(10, 8, FirstRevision) == EModelResult::WrongEpoch, TEXT("ACK from old epoch rejected"));
		Expect(Failures, Model.AcknowledgeAuthoritativeDelta(10, 9, FirstRevision + 1) == EModelResult::StaleIdentity, TEXT("Wrong ACK revision rejected"));
		Expect(Failures, Model.DiscardAuthoritativeDelta(10, 9, FirstRevision) == EModelResult::Accepted, TEXT("Failed handoff discards pending delta"));
		Expect(Failures, Model.BuildAuthoritativeDelta(11, Peer, Mapping, Due, Retry) == EModelResult::Accepted && Retry.Records.Num() == 3,
			TEXT("Unacknowledged baseline retries full initial state"));
		Expect(Failures, Model.AcknowledgeAuthoritativeDelta(10, 9, Retry.PublicationRevision) == EModelResult::Accepted, TEXT("Acknowledgement advances baseline"));
		Expect(Failures, Model.BuildAuthoritativeDelta(12, Peer, Mapping, Due, Batch) == EModelResult::Accepted && Batch.Records.IsEmpty(),
			TEXT("Unchanged acknowledged state emits no delta"));
		FPeerReplicationContext Other = Peer;
		Other.ConnectionToken = 11;
		Expect(Failures, Model.BuildAuthoritativeDelta(12, Other, Mapping, Due, Batch) == EModelResult::Accepted &&
			!Batch.Records.ContainsByPredicate([PrivateWire](const FStateRecord& R) { return R.ComponentWireId == PrivateWire; }), TEXT("Other peer never receives private component"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(11, 9, Batch.PublicationRevision);
		// Mutate only the test fixture's worker-owned component store.
		for (TPair<uint64, TArray<uint8>>& Pair : State->PublicValues) if (Pair.Value.Num() == 1 && Pair.Value[0] == 7) Pair.Value[0] = 9;
		Due[0].bAllVisibleComponentsDue = false;
		Due[0].DueComponentStableIds = { TEXT("test:private") };
		Expect(Failures, Model.BuildAuthoritativeDelta(13, Peer, Mapping, Due, Batch) == EModelResult::Accepted && Batch.Records.IsEmpty(),
			TEXT("Changed component is held until selected due"));
		Due[0].DueComponentStableIds = { TEXT("test:public") };
		Expect(Failures, Model.BuildAuthoritativeDelta(14, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.Num() == 1 && Batch.Records[0].ComponentWireId == PublicWire, TEXT("Selected changed component is sent"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Expect(Failures, Model.SetOwnerAuthoritative(Identity, 11) == EModelResult::Accepted, TEXT("Change owner"));
		Expect(Failures, Model.BuildAuthoritativeDelta(15, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([PrivateWire](const FStateRecord& R) { return R.Operation == EStateOperation::ComponentRemove && R.ComponentWireId == PrivateWire; }),
			TEXT("Ownership loss sends explicit private component removal"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Peer.RelevantEntityKeys.Empty();
		Due.Empty();
		Expect(Failures, Model.BuildAuthoritativeDelta(16, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([](const FStateRecord& R) { return R.Operation == EStateOperation::EntityLeaveRelevance; }), TEXT("Relevance exit differs from despawn"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Peer.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Identity.NetworkEntityId, Identity.Generation));
		Expect(Failures, Model.BuildAuthoritativeDelta(17, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([](const FStateRecord& R) { return R.Operation == EStateOperation::EntityCreate; }), TEXT("Re-entry resends initial state"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Expect(Failures, Model.DespawnAuthoritative(Identity, 18) == EModelResult::Accepted, TEXT("Authoritative despawn"));
		Peer.RelevantEntityKeys.Empty();
		Expect(Failures, Model.BuildAuthoritativeDelta(18, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([](const FStateRecord& R) { return R.Operation == EStateOperation::EntityDespawn; }), TEXT("Despawn emits distinct operation"));
		bool bWithinPeerLimit = true;
		for (int32 Index = 0; Index < MaxPeerReplicationStates - 2; ++Index)
		{
			FPeerReplicationContext Extra;
			Extra.ConnectionToken = static_cast<uint64>(100 + Index);
			Extra.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Broken.NetworkEntityId, Broken.Generation));
			bWithinPeerLimit &= Model.BuildAuthoritativeDelta(19, Extra, Mapping, Due, Batch) == EModelResult::Accepted;
		}
		Expect(Failures, bWithinPeerLimit, TEXT("Peers below state limit are accepted"));
		FPeerReplicationContext ExcessPeer;
		ExcessPeer.ConnectionToken = 999;
		ExcessPeer.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Broken.NetworkEntityId, Broken.Generation));
		Expect(Failures, Model.BuildAuthoritativeDelta(20, ExcessPeer, Mapping, Due, Batch) == EModelResult::LimitReached,
			TEXT("Peer replication state has a bounded limit"));
		Model.Shutdown();
		return Failures;
	});
	for (const FString& Failure : Future.Get()) AddError(Failure);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorkerDynamicComponentTest, "Flecs.Simulation.DynamicComponentContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorkerDynamicComponentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TFuture<TArray<FString>> Future = Async(EAsyncExecution::Thread, []()
	{
		TArray<FString> Failures;
		auto State = MakeShared<FTestState>();
		FRegistry Registry;
		if (!RegisterFixture(Registry, State)) { Failures.Add(TEXT("Dynamic component fixture registration")); return Failures; }
		const FCompatibilityResult Compatibility = Registry.CompareRequired(Registry);
		if (!Compatibility.bCompatible) { Failures.Add(TEXT("Dynamic component manifest")); return Failures; }
		const FSessionMapping& Mapping = Compatibility.SessionMapping;
		uint16 PrivateWire = 0;
		Mapping.FindWireId(EEntryKind::Component, TEXT("test:private"), PrivateWire);
		flecs::world World;
		FWorkerEntityModel Model(Registry, true);
		if (Model.Initialize(World, 9, [](uint64, const FString&, uint64, uint32, uint16) { return true; }) != EModelResult::Accepted)
		{ Failures.Add(TEXT("Dynamic model initialize")); return Failures; }
		TArray<FInitialComponent> Initial = { { TEXT("test:public"), { 7 } } };
		FNetworkEntityIdentity Identity;
		if (Model.SpawnAuthoritative(TEXT("test:entity"), 10, Initial, Identity) != EModelResult::Accepted)
		{ Failures.Add(TEXT("Dynamic model spawn")); Model.Shutdown(); return Failures; }
		FPeerReplicationContext Peer;
		Peer.ConnectionToken = 10;
		Peer.RelevantEntityKeys.Add(FPeerReplicationContext::MakeEntityViewKey(Identity.NetworkEntityId, Identity.Generation));
		FSelectedEntityComponents Selected;
		Selected.Identity = Identity;
		TArray<FSelectedEntityComponents> Due = { Selected };
		FStateBatch Batch;
		Expect(Failures, Model.BuildAuthoritativeDelta(1, Peer, Mapping, Due, Batch) == EModelResult::Accepted && Batch.PublicationRevision != 0,
			TEXT("Initial baseline generated"));
		Expect(Failures, Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision) == EModelResult::Accepted, TEXT("Initial baseline acknowledged"));
		TArray<uint8> Valid = { 8 };
		TArray<uint8> Invalid = { 255 };
		TArray<uint8> Oversize = { 1, 2, 3, 4, 5 };
		FNetworkEntityIdentity Stale = Identity;
		++Stale.Generation;
		Expect(Failures, Model.AddComponentAuthoritative(Stale, TEXT("test:private"), Valid, 2) == EModelResult::StaleIdentity, TEXT("Stale identity cannot mutate"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:unknown"), Valid, 2) == EModelResult::UnknownSchema, TEXT("Unregistered component rejected"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Oversize, 2) == EModelResult::UnknownSchema, TEXT("Oversized component payload rejected"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Invalid, 2) == EModelResult::CallbackFailed && State->PrivateValues.IsEmpty(),
			TEXT("Failed add removes partially applied state"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Valid, 2) == EModelResult::Accepted, TEXT("Add allowed component"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Valid, 3) == EModelResult::Invalid, TEXT("Duplicate add rejected"));
		Expect(Failures, Model.BuildAuthoritativeDelta(2, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([PrivateWire](const FStateRecord& R) { return R.Operation == EStateOperation::ComponentUpsert && R.ComponentWireId == PrivateWire; }), TEXT("Dynamic add emits upsert"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Expect(Failures, Model.UpdateComponentAuthoritative(Identity, TEXT("test:private"), Valid, 2) == EModelResult::StaleTick, TEXT("Same-tick update rejected"));
		Expect(Failures, Model.UpdateComponentAuthoritative(Identity, TEXT("test:private"), Invalid, 3) == EModelResult::CallbackFailed &&
			State->PrivateValues.Num() == 1 && State->PrivateValues.CreateConstIterator().Value()[0] == 8,
			TEXT("Failed update restores previous bytes"));
		TArray<uint8> Updated = { 9 };
		Expect(Failures, Model.UpdateComponentAuthoritative(Identity, TEXT("test:private"), Updated, 3) == EModelResult::Accepted, TEXT("Update allowed component"));
		Expect(Failures, Model.BuildAuthoritativeDelta(3, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([PrivateWire](const FStateRecord& R) { return R.Operation == EStateOperation::ComponentUpsert && R.ComponentWireId == PrivateWire && R.Payload.Num() == 1 && R.Payload[0] == 9; }),
			TEXT("Dynamic update emits new value"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Expect(Failures, Model.RemoveComponentAuthoritative(Identity, TEXT("test:private"), 3) == EModelResult::StaleTick, TEXT("Same-tick remove rejected"));
		Expect(Failures, Model.RemoveComponentAuthoritative(Identity, TEXT("test:private"), 4) == EModelResult::Accepted && State->PrivateValues.IsEmpty(), TEXT("Remove clears worker component"));
		Expect(Failures, Model.BuildAuthoritativeDelta(4, Peer, Mapping, Due, Batch) == EModelResult::Accepted &&
			Batch.Records.ContainsByPredicate([PrivateWire](const FStateRecord& R) { return R.Operation == EStateOperation::ComponentRemove && R.ComponentWireId == PrivateWire; }),
			TEXT("Acknowledged baseline emits explicit removal"));
		if (Batch.PublicationRevision) Model.AcknowledgeAuthoritativeDelta(10, 9, Batch.PublicationRevision);
		Expect(Failures, Model.UpdateComponentAuthoritative(Identity, TEXT("test:private"), Valid, 5) == EModelResult::Invalid, TEXT("Update of removed component rejected"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Valid, 4) == EModelResult::StaleTick, TEXT("Same-tick re-add rejected"));
		Expect(Failures, Model.AddComponentAuthoritative(Identity, TEXT("test:private"), Valid, 5) == EModelResult::Accepted, TEXT("Later re-add succeeds"));
		Model.Shutdown();
		return Failures;
	});
	for (const FString& Failure : Future.Get()) AddError(Failure);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorkerDuplicateDespawnTest, "Flecs.Simulation.DuplicateDespawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorkerDuplicateDespawnTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TFuture<TArray<FString>> Future = Async(EAsyncExecution::Thread, []()
	{
		TArray<FString> Failures;
		// Each conflict gets a fresh client: stale mutations can invalidate the session.
		for (int32 Case = 0; Case < 5; ++Case)
		{
			auto State = MakeShared<FTestState>();
			FRegistry Registry;
			if (!RegisterFixture(Registry, State)) { Failures.Add(TEXT("Despawn fixture registration")); break; }
			const FCompatibilityResult Compatibility = Registry.CompareRequired(Registry);
			const FSessionMapping& Mapping = Compatibility.SessionMapping;
			uint16 DefinitionWire = 0, AlternateWire = 0, PublicWire = 0;
			if (!Compatibility.bCompatible ||
				!Mapping.FindWireId(EEntryKind::EntityDefinition, TEXT("test:entity"), DefinitionWire) ||
				!Mapping.FindWireId(EEntryKind::EntityDefinition, TEXT("test:alternate"), AlternateWire) ||
				!Mapping.FindWireId(EEntryKind::Component, TEXT("test:public"), PublicWire))
			{ Failures.Add(TEXT("Despawn fixture wire mapping")); break; }
			flecs::world World;
			FWorkerEntityModel Client(Registry, false);
			if (Client.Initialize(World, 9) != EModelResult::Accepted) { Failures.Add(TEXT("Despawn client initialize")); break; }
			FStateBatch Spawn;
			Spawn.ServerWorldEpoch = 9;
			Spawn.SimulationTick = 10;
			Spawn.Records = { Record(1, 1, DefinitionWire, 0, EStateOperation::EntityCreate, 10),
				Record(1, 1, DefinitionWire, PublicWire, EStateOperation::ComponentUpsert, 10, 7) };
			Expect(Failures, Client.ApplyClientBatch(Spawn, Mapping) == EModelResult::Accepted, TEXT("Replica spawn before despawn"));
			FNetworkEntityIdentity Identity{ 9, 1, 1, TEXT("test:entity") };
			TArray<uint8> Inspected;
			Expect(Failures, Client.InspectComponentBytesForTesting(Identity, TEXT("test:public"), Inspected) == EModelResult::Accepted &&
				Inspected.Num() == 1 && Inspected[0] == 7, TEXT("Worker-only probe returns copied replica bytes"));
			FStateBatch Despawn;
			Despawn.ServerWorldEpoch = 9;
			Despawn.SimulationTick = 11;
			Despawn.Records = { Record(1, 1, DefinitionWire, 0, EStateOperation::EntityDespawn, 11) };
			Expect(Failures, Client.ApplyClientBatch(Despawn, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 0,
				TEXT("Initial true despawn succeeds"));
			const uint32 RemovalCount = State->Removals;
			Expect(Failures, Client.ApplyClientBatch(Despawn, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 0 && State->Removals == RemovalCount,
				TEXT("Lost ACK exact despawn retry is idempotent"));
			FStateBatch Conflict = Despawn;
			if (Case == 0) { Conflict.ServerWorldEpoch = 8; Conflict.Records[0].Identity.ServerWorldEpoch = 8; }
			if (Case == 1) { Conflict.SimulationTick = 12; Conflict.Records[0].SourceSimulationTick = 12; }
			if (Case == 2) { Conflict.Records[0].Identity.Generation = 2; }
			if (Case == 3) { Conflict.Records[0].DefinitionWireId = AlternateWire; }
			if (Case == 4)
			{
				FStateBatch Newer = Spawn;
				Newer.SimulationTick = 12;
				for (FStateRecord& Item : Newer.Records) { Item.Identity.Generation = 2; Item.SourceSimulationTick = 12; }
				Expect(Failures, Client.ApplyClientBatch(Newer, Mapping) == EModelResult::Accepted && Client.GetEntityCount() == 1,
					TEXT("Newer incarnation creates safely after despawn"));
				Conflict.SimulationTick = 12;
			}
			Expect(Failures, Client.ApplyClientBatch(Conflict, Mapping) != EModelResult::Accepted,
				TEXT("Conflicting or stale despawn rejected"));
			Expect(Failures, Client.GetEntityCount() == (Case == 4 ? 1u : 0u), TEXT("Rejected despawn cannot resurrect or remove a newer identity"));
			Client.Shutdown();
		}
		return Failures;
	});
	for (const FString& Failure : Future.Get()) AddError(Failure);
	return true;
}

#endif
