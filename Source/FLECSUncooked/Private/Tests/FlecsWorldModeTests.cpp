#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "FlecsSimulationTypes.h"
#include "Misc/AutomationTest.h"
#include "Networking/FlecsWorldMode.h"
#include "Simulation/FlecsWorkerEntityModel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsAuthoritativeWorldModesTest, "Flecs.Network.AuthoritativeWorldModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsAuthoritativeWorldModesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TestTrue(TEXT("Standalone owns simulation"), FlecsHasAuthoritativeSimulation(NM_Standalone));
	TestTrue(TEXT("Dedicated server owns simulation"), FlecsHasAuthoritativeSimulation(NM_DedicatedServer));
	TestTrue(TEXT("Listen server owns simulation"), FlecsHasAuthoritativeSimulation(NM_ListenServer));
	TestFalse(TEXT("Network client is not authoritative"), FlecsHasAuthoritativeSimulation(NM_Client));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsSharedAuthoritativeEntityTest, "Flecs.Network.SharedAuthoritativeEntity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsSharedAuthoritativeEntityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TFuture<TArray<FString>> Future = Async(EAsyncExecution::Thread, []() {
		using namespace FlecsModRegistry;
		using namespace FlecsSimulation;
		TArray<FString> Errors;
		auto Expect = [&Errors](bool Condition, const TCHAR* Message) {
			if (!Condition)
				Errors.Add(Message);
		};
		TMap<uint64, TArray<uint8>> Values;
		FRegistry Registry;
		FString Error;
		FComponentSchema Component;
		Component.StableId = TEXT("example:health");
		Component.Version = {1, 0, 0};
		Component.MaxEncodedBytes = 1;
		Component.AllowedEntityDefinitions.Add(TEXT("example:entity"));
		Component.Extract = [&Values](uint64 Token, TArray<uint8>& Out) {
			const TArray<uint8>* Value = Values.Find(Token);
			if (!Value)
				return false;
			Out = *Value;
			return true;
		};
		Component.Apply = [&Values](uint64 Token, TConstArrayView<uint8> Bytes) {
			if (Bytes.Num() != 1)
				return false;
			TArray<uint8>& Value = Values.FindOrAdd(Token);
			Value.Reset();
			Value.Append(Bytes.GetData(), Bytes.Num());
			return true;
		};
		Component.Remove = [&Values](uint64 Token) {
			Values.Remove(Token);
		};
		if (!Registry.RegisterComponent(MoveTemp(Component), Error))
		{
			Errors.Add(Error);
			return Errors;
		}
		FEntityDefinition Definition;
		Definition.StableId = TEXT("example:entity");
		Definition.Version = {1, 0, 0};
		Definition.ComponentIds.Add(TEXT("example:health"));
		Definition.MaxInitialStateBytes = 1;
		Definition.Create = [](uint64 Token) {
			return Token != 0;
		};
		if (!Registry.RegisterEntityDefinition(MoveTemp(Definition), Error) || !Registry.Freeze(1, Error))
		{
			Errors.Add(Error);
			return Errors;
		}
		flecs::world World;
		FWorkerEntityModel Model(Registry, true);
		if (Model.Initialize(World, 41, [](uint64, const FString&, uint64, uint32, uint16) { return false; })
			!= EModelResult::Accepted)
		{
			Errors.Add(TEXT("Initialize authoritative model"));
			return Errors;
		}
		TArray<FInitialComponent> Initial = {{TEXT("example:health"), {100}}};
		FNetworkEntityIdentity Identity;
		const EModelResult Spawn = Model.SpawnAuthoritative(TEXT("example:entity"), 0, Initial, Identity);
		Expect(Spawn == EModelResult::Accepted, TEXT("Unowned shared spawn succeeds"));
		Expect(
			Identity.ServerWorldEpoch == 41 && Identity.NetworkEntityId != 0, TEXT("Spawn has authoritative identity"));
		TArray<FNetworkEntityIdentity> Live;
		Expect(Model.BuildLiveEntityIdentities(Live) == EModelResult::Accepted && Live.Num() == 1,
			TEXT("Shared entity is live"));
		Expect(Model.DespawnAuthoritative(Identity, 1) == EModelResult::Accepted, TEXT("Shared entity despawns"));
		Expect(Model.BuildLiveEntityIdentities(Live) == EModelResult::Accepted && Live.IsEmpty(),
			TEXT("Entity removed from live set"));
		Model.Shutdown();
		return Errors;
	});
	for (const FString& Error : Future.Get())
		AddError(Error);
	return true;
}

#endif
