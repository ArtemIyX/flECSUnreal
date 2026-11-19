#include "Quantization/FlecsQuantization.h"

#include <cmath>
#include <limits>

bool FlecsQuantization::QuantizeSigned(double Value, double Step, int32& OutValue)
{
	if (!std::isfinite(Value) || !std::isfinite(Step) || Step <= 0.0) return false;
	const double Scaled = Value / Step;
	if (!std::isfinite(Scaled)) return false;
	const double Rounded = std::round(Scaled);
	if (Rounded < static_cast<double>(MIN_int32) || Rounded > static_cast<double>(MAX_int32)) return false;
	OutValue = static_cast<int32>(Rounded);
	return true;
}

bool FlecsQuantization::DequantizeSigned(int32 Value, double Step, double& OutValue)
{
	if (!std::isfinite(Step) || Step <= 0.0) return false;
	const double Result = static_cast<double>(Value) * Step;
	if (!std::isfinite(Result)) return false;
	OutValue = Result;
	return true;
}

bool FlecsQuantization::QuantizeAngleDegrees(double Degrees, uint16& OutValue)
{
	if (!std::isfinite(Degrees)) return false;
	double Wrapped = std::fmod(Degrees, 360.0);
	if (Wrapped < 0.0) Wrapped += 360.0;
	const uint32 Encoded = static_cast<uint32>(std::floor(Wrapped * (65536.0 / 360.0) + 0.5));
	OutValue = static_cast<uint16>(Encoded & 0xffffu);
	return true;
}

double FlecsQuantization::DequantizeAngleDegrees(uint16 Value)
{
	return static_cast<double>(Value) * (360.0 / 65536.0);
}

void FlecsQuantization::WriteInt32(TArray<uint8>& Bytes, int32 Value)
{
	const uint32 Bits = static_cast<uint32>(Value);
	Bytes.Add(static_cast<uint8>(Bits >> 24)); Bytes.Add(static_cast<uint8>(Bits >> 16));
	Bytes.Add(static_cast<uint8>(Bits >> 8)); Bytes.Add(static_cast<uint8>(Bits));
}

void FlecsQuantization::WriteUInt16(TArray<uint8>& Bytes, uint16 Value)
{
	Bytes.Add(static_cast<uint8>(Value >> 8)); Bytes.Add(static_cast<uint8>(Value));
}

bool FlecsQuantization::ReadInt32(TConstArrayView<uint8> Bytes, int32& Offset, int32& OutValue)
{
	if (Offset < 0 || Offset > Bytes.Num() || Bytes.Num() - Offset < 4) return false;
	const uint32 Bits = (static_cast<uint32>(Bytes[Offset]) << 24) | (static_cast<uint32>(Bytes[Offset + 1]) << 16) |
		(static_cast<uint32>(Bytes[Offset + 2]) << 8) | static_cast<uint32>(Bytes[Offset + 3]);
	const int64 Signed = Bits <= static_cast<uint32>(MAX_int32) ? static_cast<int64>(Bits) : static_cast<int64>(Bits) - 4294967296LL;
	OutValue = static_cast<int32>(Signed);
	Offset += 4;
	return true;
}

bool FlecsQuantization::ReadUInt16(TConstArrayView<uint8> Bytes, int32& Offset, uint16& OutValue)
{
	if (Offset < 0 || Offset > Bytes.Num() || Bytes.Num() - Offset < 2) return false;
	OutValue = static_cast<uint16>((static_cast<uint16>(Bytes[Offset]) << 8) | Bytes[Offset + 1]);
	Offset += 2;
	return true;
}
