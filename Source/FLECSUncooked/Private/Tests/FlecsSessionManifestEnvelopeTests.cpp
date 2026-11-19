#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Networking/FlecsSessionManifestEnvelope.h"
#include "ModRegistry/ModSchemaRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsSessionManifestEnvelopeTest, "Flecs.Network.SessionManifestEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsSessionManifestEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<uint8> Manifest = { 0, 1, 2, 255 };
	TArray<uint8> Encoded, Decoded;
	uint32 Capacity = 0;
	TestTrue(TEXT("Maximum capacity is encodable"),
		FFlecsSessionManifestEnvelope::Encode(100000, Manifest, Encoded));
	TestTrue(TEXT("Envelope decodes"), FFlecsSessionManifestEnvelope::Decode(Encoded, Capacity, Decoded));
	TestEqual(TEXT("Capacity survives round trip"), Capacity, 100000u);
	TestTrue(TEXT("Manifest survives round trip"), Decoded == Manifest);
	FString Error;
	TestTrue(TEXT("Matching session capacity is accepted"),
		FFlecsSessionManifestEnvelope::DecodeMatching(Encoded, 100000, Decoded, Error));
	TestFalse(TEXT("Mismatched session capacity is rejected"),
		FFlecsSessionManifestEnvelope::DecodeMatching(Encoded, 5000, Decoded, Error));
	TestTrue(TEXT("Capacity mismatch has a diagnostic"), Error.Contains(TEXT("capacity mismatch")));
	TestTrue(TEXT("Capacity mismatch clears manifest"), Decoded.IsEmpty());

	TArray<uint8> Invalid = Encoded;
	Invalid[7] = 2;
	TestFalse(TEXT("Unknown envelope version is rejected"),
		FFlecsSessionManifestEnvelope::Decode(Invalid, Capacity, Decoded));
	TestEqual(TEXT("Failed decode clears capacity"), Capacity, 0u);
	TestTrue(TEXT("Failed decode clears manifest"), Decoded.IsEmpty());
	Invalid = Encoded;
	Invalid.SetNum(FFlecsSessionManifestEnvelope::HeaderBytes);
	TestFalse(TEXT("Truncated manifest is rejected"),
		FFlecsSessionManifestEnvelope::Decode(Invalid, Capacity, Decoded));
	TestFalse(TEXT("Zero capacity is rejected"), FFlecsSessionManifestEnvelope::Encode(0, Manifest, Invalid));
	TestFalse(TEXT("Over-limit capacity is rejected"),
		FFlecsSessionManifestEnvelope::Encode(100001, Manifest, Invalid));
	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(FlecsModRegistry::MaxManifestDescriptorBytes);
	TestFalse(TEXT("Transport envelope size is bounded"),
		FFlecsSessionManifestEnvelope::Encode(5000, Oversized, Invalid));
	return true;
}

#endif
