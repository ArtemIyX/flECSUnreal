#if WITH_DEV_AUTOMATION_TESTS

#include "FlecsBoundedPayload.h"
#include "FlecsBoundedPayloadNetSerializer.h"
#include "Iris/ReplicationState/PropertyNetSerializerInfoRegistry.h"
#include "Iris/Serialization/NetBitStreamReader.h"
#include "Iris/Serialization/NetBitStreamWriter.h"
#include "Iris/Serialization/NetSerializationContext.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlecsBoundedPayloadSerializerTest, "Flecs.Network.Iris.BoundedPayloadSerializer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlecsBoundedPayloadSerializerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::Net;
	const FNetSerializer& Serializer = UE_NET_GET_SERIALIZER(FFlecsBoundedPayloadNetSerializer);
	const FPropertyNetSerializerInfo* Info = FPropertyNetSerializerInfoRegistry::FindStructSerializerInfo(TEXT("FlecsBoundedPayload"));
	TestTrue(TEXT("Named Iris serializer is registered"), Info && Info->GetNetSerializer(nullptr) == &Serializer);
	alignas(16) uint8 Quantized[sizeof(FFlecsBoundedPayload) + 16] = {};
	alignas(16) uint8 Decoded[sizeof(FFlecsBoundedPayload) + 16] = {};
	if (!TestTrue(TEXT("Quantized state fits aligned test buffer"), Serializer.QuantizedTypeSize <= sizeof(Quantized) && Serializer.QuantizedTypeAlignment <= 16)) return false;

	for (const int32 Size : { 0, 1, static_cast<int32>(FFlecsBoundedPayload::Capacity) })
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index) Bytes[Index] = static_cast<uint8>((Index * 37) & 0xff);
		FFlecsBoundedPayload Input, Output;
		TestTrue(TEXT("Create bounded source"), Input.SetBytes(Bytes.GetData(), Bytes.Num()));
		FMemory::Memzero(Quantized, sizeof(Quantized));
		FNetSerializationContext QuantizeContext;
		FNetQuantizeArgs QuantizeArgs = {};
		QuantizeArgs.Version = Serializer.Version;
		QuantizeArgs.NetSerializerConfig = Serializer.DefaultConfig;
		QuantizeArgs.Source = NetSerializerValuePointer(&Input);
		QuantizeArgs.Target = NetSerializerValuePointer(Quantized);
		Serializer.Quantize(QuantizeContext, QuantizeArgs);
		TestFalse(TEXT("Quantize accepts bounded payload"), QuantizeContext.HasError());

		uint32 Stream[260] = {};
		FNetBitStreamWriter Writer;
		Writer.InitBytes(Stream, sizeof(Stream));
		FNetSerializationContext WriteContext(&Writer);
		FNetSerializeArgs WriteArgs = {};
		WriteArgs.Version = Serializer.Version;
		WriteArgs.NetSerializerConfig = Serializer.DefaultConfig;
		WriteArgs.Source = NetSerializerValuePointer(Quantized);
		Serializer.Serialize(WriteContext, WriteArgs);
		Writer.CommitWrites();
		const uint32 BitCount = Writer.GetPosBits();
		TestFalse(TEXT("Iris serialization succeeds"), WriteContext.HasErrorOrOverflow());
		TestEqual(TEXT("Encoded payload uses bounded size prefix"), BitCount, static_cast<uint32>(11 + Size * 8));

		FMemory::Memzero(Decoded, sizeof(Decoded));
		FNetBitStreamReader Reader;
		Reader.InitBits(Stream, BitCount);
		FNetSerializationContext ReadContext(&Reader);
		FNetDeserializeArgs ReadArgs = {};
		ReadArgs.Version = Serializer.Version;
		ReadArgs.NetSerializerConfig = Serializer.DefaultConfig;
		ReadArgs.Target = NetSerializerValuePointer(Decoded);
		Serializer.Deserialize(ReadContext, ReadArgs);
		TestFalse(TEXT("Iris deserialization succeeds"), ReadContext.HasErrorOrOverflow());
		FNetSerializationContext DequantizeContext;
		FNetDequantizeArgs DequantizeArgs = {};
		DequantizeArgs.Version = Serializer.Version;
		DequantizeArgs.NetSerializerConfig = Serializer.DefaultConfig;
		DequantizeArgs.Source = NetSerializerValuePointer(Decoded);
		DequantizeArgs.Target = NetSerializerValuePointer(&Output);
		Serializer.Dequantize(DequantizeContext, DequantizeArgs);
		TestTrue(TEXT("Iris round trip preserves payload"), !DequantizeContext.HasError() && Output == Input);

		if (Size > 0)
		{
			FNetBitStreamReader TruncatedReader;
			TruncatedReader.InitBits(Stream, BitCount - 1);
			FNetSerializationContext TruncatedContext(&TruncatedReader);
			Serializer.Deserialize(TruncatedContext, ReadArgs);
			TestTrue(TEXT("Truncated payload fails"), TruncatedContext.HasErrorOrOverflow());
		}
	}

	uint32 BadStream[2] = {};
	FNetBitStreamWriter BadWriter;
	BadWriter.InitBytes(BadStream, sizeof(BadStream));
	BadWriter.WriteBits(FFlecsBoundedPayload::Capacity + 1, 11);
	BadWriter.CommitWrites();
	FNetBitStreamReader BadReader;
	BadReader.InitBits(BadStream, 11);
	FNetSerializationContext BadContext(&BadReader);
	FNetDeserializeArgs BadArgs = {};
	BadArgs.Version = Serializer.Version;
	BadArgs.NetSerializerConfig = Serializer.DefaultConfig;
	BadArgs.Target = NetSerializerValuePointer(Decoded);
	Serializer.Deserialize(BadContext, BadArgs);
	TestTrue(TEXT("Oversized wire length rejected before payload decode"), BadContext.HasError());
	FNetBitStreamReader ShortPrefixReader;
	ShortPrefixReader.InitBits(BadStream, 10);
	FNetSerializationContext ShortPrefixContext(&ShortPrefixReader);
	Serializer.Deserialize(ShortPrefixContext, BadArgs);
	TestTrue(TEXT("Truncated length prefix rejected"), ShortPrefixContext.HasErrorOrOverflow());

	FFlecsBoundedPayload Invalid;
	Invalid.NumBytes = FFlecsBoundedPayload::Capacity + 1;
	FNetSerializationContext ValidateContext;
	FNetValidateArgs ValidateArgs = {};
	ValidateArgs.Version = Serializer.Version;
	ValidateArgs.NetSerializerConfig = Serializer.DefaultConfig;
	ValidateArgs.Source = NetSerializerValuePointer(&Invalid);
	TestFalse(TEXT("Oversized source fails Iris validation"), Serializer.Validate(ValidateContext, ValidateArgs));
	return true;
}

#endif
