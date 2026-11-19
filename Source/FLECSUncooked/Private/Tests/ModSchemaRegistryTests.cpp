#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "ModRegistry/ModSchemaRegistry.h"

using namespace FlecsModRegistry;

namespace
{
FComponentSchema MakeComponent(const TCHAR* Id, uint16 Major = 1)
{
	FComponentSchema Component;
	Component.StableId = Id;
	Component.Version.Major = Major;
	Component.MaxEncodedBytes = 64;
	Component.Extract = [](uint64, TArray<uint8>&) {
		return true;
	};
	Component.Apply = [](uint64, TConstArrayView<uint8>) {
		return true;
	};
	Component.Remove = [](uint64) {
	};
	return Component;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModRegistryCompatibilityTest, "Flecs.Mod.RegistryCompatibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModRegistryCompatibilityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	FRegistry Server;
	FRegistry CompatibleClient;
	FRegistry IncompatibleClient;
	TestTrue(TEXT("Valid schema registered"), Server.RegisterComponent(MakeComponent(TEXT("example:health")), Error));
	TestFalse(TEXT("Duplicate IDs rejected"), Server.RegisterComponent(MakeComponent(TEXT("example:health")), Error));
	TestFalse(TEXT("Unnamespaced ID rejected"), Server.RegisterComponent(MakeComponent(TEXT("health")), Error));
	TestTrue(TEXT("Matching schema registered"),
		CompatibleClient.RegisterComponent(MakeComponent(TEXT("example:health")), Error));
	TestTrue(TEXT("Different version registered"),
		IncompatibleClient.RegisterComponent(MakeComponent(TEXT("example:health"), 2), Error));
	TestTrue(TEXT("Server registry freezes"), Server.Freeze(1, Error));
	TestTrue(TEXT("Matching registry freezes"), CompatibleClient.Freeze(1, Error));
	TestTrue(TEXT("Mismatching registry freezes"), IncompatibleClient.Freeze(1, Error));
	TestFalse(TEXT("Frozen registry rejects registrations"),
		Server.RegisterComponent(MakeComponent(TEXT("example:mana")), Error));
	const FCompatibilityResult Good = Server.CompareRequired(CompatibleClient);
	const FCompatibilityResult Bad = Server.CompareRequired(IncompatibleClient);
	TestTrue(TEXT("Identical manifests match"), Good.bCompatible);
	TestFalse(TEXT("Version mismatch is rejected"), Bad.bCompatible);
	TestTrue(TEXT("Mismatch supplies diagnostic"), !Bad.Diagnostics.IsEmpty());
	uint16 GoodId = 0;
	uint16 BadId = 0;
	TestTrue(TEXT("Compatible peer obtains wire ID"),
		Good.SessionMapping.FindWireId(EEntryKind::Component, TEXT("example:health"), GoodId));
	TestTrue(TEXT("Wire ID is nonzero"), GoodId != 0);
	TestFalse(TEXT("Incompatible peer obtains no wire ID"),
		Bad.SessionMapping.FindWireId(EEntryKind::Component, TEXT("example:health"), BadId));
	TestTrue(TEXT("Failed peer cannot invalidate another peer's mapping"),
		Good.SessionMapping.FindWireId(EEntryKind::Component, TEXT("example:health"), GoodId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModManifestCodecTest, "Flecs.Mod.ManifestCodec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModManifestCodecTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	FRegistry Server;
	if (!TestTrue(TEXT("Register schema"), Server.RegisterComponent(MakeComponent(TEXT("example:health")), Error))
		|| !TestTrue(TEXT("Freeze registry"), Server.Freeze(7, Error)))
		return false;

	FManifestDescriptor Original;
	if (!TestTrue(TEXT("Export manifest"), Server.ExportManifestDescriptor(Original, Error)))
		return false;
	TArray<uint8> Encoded;
	if (!TestTrue(TEXT("Encode manifest"), Original.Serialize(Encoded, Error)))
		return false;
	FManifestDescriptor Decoded;
	if (!TestTrue(TEXT("Decode manifest"), FManifestDescriptor::Deserialize(Encoded, Decoded, Error)))
		return false;
	TestEqual(TEXT("Protocol survives round trip"), Decoded.ProtocolVersion, uint32(7));
	TestEqual(TEXT("Entry count survives round trip"), Decoded.Entries.Num(), 1);
	const FCompatibilityResult Compatible = Server.CompareRequired(Decoded);
	TestTrue(TEXT("Decoded manifest is compatible"), Compatible.bCompatible);
	uint16 WireId = 0;
	FSessionWireEntry Reverse;
	TestTrue(TEXT("Find mapped component"),
		Compatible.SessionMapping.FindWireId(EEntryKind::Component, TEXT("example:health"), WireId));
	TestTrue(TEXT("Reverse wire lookup succeeds"), Compatible.SessionMapping.FindEntryByWireId(WireId, Reverse));
	TestTrue(TEXT("Reverse lookup returns component kind"), Reverse.Kind == EEntryKind::Component);
	TestEqual(TEXT("Reverse lookup returns stable ID"), Reverse.StableId, FString(TEXT("example:health")));
	TestFalse(TEXT("Zero wire ID is invalid"), Compatible.SessionMapping.FindEntryByWireId(0, Reverse));

	FManifestDescriptor Mismatch = Decoded;
	Mismatch.Entries[0].Version.Major++;
	const FCompatibilityResult Incompatible = Server.CompareRequired(Mismatch);
	TestFalse(TEXT("Decoded schema mismatch rejected"), Incompatible.bCompatible);
	TestFalse(
		TEXT("Incompatible peer cannot reverse IDs"), Incompatible.SessionMapping.FindEntryByWireId(WireId, Reverse));
	TestTrue(TEXT("Failed comparison preserves other peer mapping"),
		Compatible.SessionMapping.FindEntryByWireId(WireId, Reverse));

	for (int32 Length = 0; Length < Encoded.Num(); ++Length)
	{
		FManifestDescriptor Output;
		Output.ProtocolVersion = 99;
		if (FManifestDescriptor::Deserialize(MakeArrayView(Encoded.GetData(), Length), Output, Error)
			|| Output.ProtocolVersion != 99)
		{
			AddError(FString::Printf(TEXT("Truncated input of length %d was accepted or changed output."), Length));
			break;
		}
	}

	TArray<uint8> Malformed = Encoded;
	Malformed[0] = 0;
	TestFalse(TEXT("Bad magic rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));
	Malformed = Encoded;
	Malformed[10] = 0xff; // Entry kind follows the ten-byte descriptor header.
	TestFalse(TEXT("Invalid entry kind rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));
	Malformed = Encoded;
	Malformed[8] = 0xff;
	Malformed[9] = 0xff;
	TestFalse(TEXT("Excessive entry count rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));
	Malformed = Encoded;
	Malformed.Add(0);
	TestFalse(TEXT("Trailing bytes rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));
	Malformed = Encoded;
	Malformed[8] = 2;
	Malformed[9] = 0;
	Malformed.Append(Encoded.GetData() + 10, Encoded.Num() - 10);
	TestFalse(TEXT("Duplicate wire entries rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));
	Malformed.SetNumZeroed(static_cast<int32>(MaxManifestDescriptorBytes) + 1);
	TestFalse(TEXT("Oversized descriptor rejected"), FManifestDescriptor::Deserialize(Malformed, Decoded, Error));

	FManifestDescriptor Invalid = Original;
	const FManifestEntry Duplicate = Invalid.Entries[0];
	Invalid.Entries.Add(Duplicate);
	TestFalse(TEXT("Duplicate descriptor IDs rejected"), Invalid.Validate(Error));
	Invalid = Original;
	Invalid.Entries[0].StableId = TEXT("INVALID");
	TestFalse(TEXT("Malformed stable ID rejected"), Invalid.Validate(Error));
	Invalid = Original;
	Invalid.Entries[0].MaxPayloadBytes = MaxComponentPayloadBytes + 1;
	TestFalse(TEXT("Excessive payload bound rejected"), Invalid.Validate(Error));
	return true;
}

#endif
