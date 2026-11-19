#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Quantization/FlecsQuantization.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsQuantizationContractTest, "Flecs.Network.Quantization.Primitives",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsQuantizationContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace FlecsQuantization;
	int32 Value = 99;
	TestTrue(TEXT("Positive half rounds away from zero"), QuantizeSigned(1.25, 0.5, Value) && Value == 3);
	TestTrue(TEXT("Negative half rounds away from zero"), QuantizeSigned(-1.25, 0.5, Value) && Value == -3);
	TestTrue(TEXT("Maximum int32 accepted"),
		QuantizeSigned(static_cast<double>(MAX_int32), 1.0, Value) && Value == MAX_int32);
	TestTrue(TEXT("Minimum int32 accepted"),
		QuantizeSigned(static_cast<double>(MIN_int32), 1.0, Value) && Value == MIN_int32);
	Value = 77;
	TestFalse(TEXT("Overflow rejected"), QuantizeSigned(static_cast<double>(MAX_int32) + 1.0, 1.0, Value));
	TestFalse(TEXT("Zero step rejected"), QuantizeSigned(1.0, 0.0, Value));
	TestFalse(TEXT("NaN rejected"), QuantizeSigned(std::numeric_limits<double>::quiet_NaN(), 1.0, Value));
	TestEqual(TEXT("Failed signed quantization preserves output"), Value, 77);
	double Reconstructed = 3.0;
	TestTrue(TEXT("Signed reconstruction"), DequantizeSigned(-3, 0.5, Reconstructed) && Reconstructed == -1.5);
	TestFalse(TEXT("Invalid reconstruction step rejected"), DequantizeSigned(1, -1.0, Reconstructed));
	TestTrue(TEXT("Failed reconstruction preserves output"), Reconstructed == -1.5);
	for (int32 Sample : {MIN_int32, -1000000, -1, 0, 1, 1000000, MAX_int32})
	{
		double Decoded = 0.0;
		int32 Reencoded = 0;
		if (!DequantizeSigned(Sample, 0.25, Decoded) || !QuantizeSigned(Decoded, 0.25, Reencoded)
			|| Reencoded != Sample)
		{
			AddError(TEXT("Signed quantized integer did not round-trip"));
			break;
		}
	}
	uint16 Angle = 0;
	TestTrue(TEXT("Quarter turn encodes"), QuantizeAngleDegrees(90.0, Angle) && Angle == 16384);
	TestTrue(TEXT("Negative angle wraps"), QuantizeAngleDegrees(-90.0, Angle) && Angle == 49152);
	TestTrue(TEXT("Full turn wraps"), QuantizeAngleDegrees(360.0, Angle) && Angle == 0);
	Angle = 42;
	TestFalse(TEXT("Non-finite angle rejected"), QuantizeAngleDegrees(std::numeric_limits<double>::infinity(), Angle));
	TestEqual(TEXT("Failed angle quantization preserves output"), Angle, uint16(42));
	for (uint32 Sample = 0; Sample <= 65535; ++Sample)
	{
		uint16 Reencoded = 0;
		if (!QuantizeAngleDegrees(DequantizeAngleDegrees(static_cast<uint16>(Sample)), Reencoded)
			|| Reencoded != Sample)
		{
			AddError(TEXT("Circular angle code did not round-trip"));
			break;
		}
	}
	TArray<uint8> Bytes;
	WriteInt32(Bytes, -2);
	WriteInt32(Bytes, MIN_int32);
	WriteUInt16(Bytes, 0x1234);
	const TArray<uint8> Expected = {0xff, 0xff, 0xff, 0xfe, 0x80, 0, 0, 0, 0x12, 0x34};
	TestTrue(TEXT("Integer bytes use network order"), Bytes == Expected);
	int32 Offset = 0, Signed = 0;
	uint16 Unsigned = 0;
	TestTrue(TEXT("Negative integer decoded"), ReadInt32(Bytes, Offset, Signed) && Signed == -2);
	TestTrue(TEXT("Minimum integer decoded"), ReadInt32(Bytes, Offset, Signed) && Signed == MIN_int32);
	TestTrue(TEXT("Unsigned integer decoded"), ReadUInt16(Bytes, Offset, Unsigned) && Unsigned == 0x1234);
	TestEqual(TEXT("Reader consumed expected bytes"), Offset, 10);
	TestFalse(TEXT("Truncated read rejected"), ReadInt32(Bytes, Offset, Signed));
	TestTrue(TEXT("Rejected read preserves offset and output"), Offset == 10 && Signed == MIN_int32);
	Offset = -1;
	TestFalse(TEXT("Negative offset rejected"), ReadUInt16(Bytes, Offset, Unsigned));
	TestTrue(TEXT("Rejected short read preserves offset and output"), Offset == -1 && Unsigned == 0x1234);
	return true;
}
#endif
