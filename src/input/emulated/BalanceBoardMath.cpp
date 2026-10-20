#include "input/emulated/BalanceBoardMath.h"

#include <algorithm>
#include <cmath>

namespace BalanceBoardMath
{
	static void PutBE16(uint8_t* p, uint16_t v)
	{
		p[0] = (uint8_t)(v >> 8);
		p[1] = (uint8_t)v;
	}

	static uint16_t GetBE16(const uint8_t* p)
	{
		return (uint16_t)((p[0] << 8) | p[1]);
	}

	ExtMemory BuildExtensionMemory()
	{
		ExtMemory mem{};
		mem[kRegCalInfo + 0] = 0x01;
		mem[kRegCalInfo + 1] = kReferenceBattery;
		for (int i = 0; i < kSensorCount; i++)
		{
			PutBE16(&mem[kRegCal0Kg + i * 2], kCal0Kg);
			PutBE16(&mem[kRegCal17Kg + i * 2], kCal17Kg);
			PutBE16(&mem[kRegCal34Kg + i * 2], kCal34Kg);
		}
		mem[kRegCalTemp + 0] = (uint8_t)kReferenceTemperature;
		mem[kRegCalTemp + 1] = kCalibrationVersion;

		const uint32_t crc = CalibrationCrc(mem);
		mem[kRegCalCrc + 0] = (uint8_t)(crc >> 24);
		mem[kRegCalCrc + 1] = (uint8_t)(crc >> 16);
		mem[kRegCalCrc + 2] = (uint8_t)(crc >> 8);
		mem[kRegCalCrc + 3] = (uint8_t)crc;

		static constexpr uint8_t kIdentifier[6] = {0x00, 0x00, 0xa4, 0x20, 0x04, 0x02};
		std::copy(std::begin(kIdentifier), std::end(kIdentifier), mem.begin() + kRegIdentifier);
		return mem;
	}

	uint32_t Crc32(const uint8_t* data, size_t length)
	{
		uint32_t crc = 0xFFFFFFFF;
		for (size_t i = 0; i < length; i++)
		{
			crc ^= data[i];
			for (int bit = 0; bit < 8; bit++)
				crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
		}
		return crc ^ 0xFFFFFFFF;
	}

	uint32_t CalibrationCrc(const ExtMemory& mem)
	{
		uint8_t buf[0x18 + 4];
		std::copy(mem.begin() + kRegCal0Kg, mem.begin() + kRegCal0Kg + 0x18, buf);
		buf[0x18] = mem[kRegCalInfo + 0];
		buf[0x19] = mem[kRegCalInfo + 1];
		buf[0x1A] = mem[kRegCalTemp + 0];
		buf[0x1B] = mem[kRegCalTemp + 1];
		return Crc32(buf, sizeof(buf));
	}

	Calibration ParseCalibration(const ExtMemory& mem)
	{
		Calibration cal{};
		for (int i = 0; i < kSensorCount; i++)
		{
			cal.raw[i][0] = GetBE16(&mem[kRegCal0Kg + i * 2]);
			cal.raw[i][1] = GetBE16(&mem[kRegCal17Kg + i * 2]);
			cal.raw[i][2] = GetBE16(&mem[kRegCal34Kg + i * 2]);
		}
		cal.ref_temperature = (int8_t)mem[kRegCalTemp + 0];
		cal.version = mem[kRegCalTemp + 1];
		const double max_load = (cal.version == 1) ? 34.0 : 25.0;
		cal.load_kg[0] = 0.0;
		cal.load_kg[1] = max_load / 2.0;
		cal.load_kg[2] = max_load;

		cal.crc_stored = (uint32_t)mem[kRegCalCrc] << 24 | (uint32_t)mem[kRegCalCrc + 1] << 16 |
			(uint32_t)mem[kRegCalCrc + 2] << 8 | (uint32_t)mem[kRegCalCrc + 3];
		cal.crc_computed = CalibrationCrc(mem);
		cal.valid = cal.crc_stored == cal.crc_computed;
		return cal;
	}

	uint16_t KgToRaw(double kg)
	{
		const double raw = std::round(kCal0Kg + kg * (kRawPerKg17 / 17.0));
		return (uint16_t)std::clamp(raw, 0.0, 65535.0);
	}

	double RawToKg(uint16_t raw)
	{
		return ((double)raw - kCal0Kg) * (17.0 / kRawPerKg17);
	}

	double RawToKg(const Calibration& cal, int sensor, uint16_t raw, double zero)
	{
		const uint16_t* points = cal.raw[sensor];
		const int seg = (raw > points[1]) ? 1 : 0;
		const double raw_per_kg = ((double)points[seg + 1] - points[seg]) / (cal.load_kg[seg + 1] - cal.load_kg[seg]);
		if (raw_per_kg == 0.0)
			return 0.0;
		// the zero point replaces the factory 0 kg point; the segment starts (points[seg] - points[0]) above it
		const double start = zero + ((double)points[seg] - points[0]);
		return cal.load_kg[seg] + ((double)raw - start) / raw_per_kg;
	}

	int32_t RawLoadAboveZeroKg(const Calibration& cal, const uint16_t raw[kSensorCount])
	{
		int32_t total = 0;
		for (int i = 0; i < kSensorCount; i++)
			total += (int32_t)raw[i] - (int32_t)cal.raw[i][0];
		return total;
	}

	double TemperatureCorrected(double weight, int8_t temperature, int8_t ref_temperature)
	{
		return weight * (1.0 - 0.007 * ((double)(temperature - ref_temperature) / 10.0));
	}

	std::array<double, kSensorCount> CenterOfBalanceToSensors(double x, double y, double total_weight)
	{
		const double q = total_weight * 0.25;
		return {(1 + x) * (1 + y) * q, (1 + x) * (1 - y) * q, (1 - x) * (1 + y) * q, (1 - x) * (1 - y) * q};
	}

	void SensorsToCenterOfBalance(const std::array<double, kSensorCount>& s, double& x, double& y)
	{
		const double right = s[kTopRight] + s[kBottomRight];
		const double left = s[kTopLeft] + s[kBottomLeft];
		const double total = right + left;
		if (total == 0.0)
		{
			x = y = 0.0;
			return;
		}
		const double top = s[kTopRight] + s[kTopLeft];
		const double bottom = s[kBottomRight] + s[kBottomLeft];
		x = (right - left) / total;
		y = (top - bottom) / total;
	}

	uint8_t BatteryPercentToRaw(int percent)
	{
		const double charge = std::clamp(percent, 0, 100) / 100.0;
		return (uint8_t)std::lround((charge + 3.53) / 8.52 * 0xFF);
	}

	int BatteryRawToLevel(uint8_t raw)
	{
		if (raw >= 0x82)
			return 4;
		if (raw >= 0x7D)
			return 3;
		if (raw >= 0x78)
			return 2;
		if (raw >= 0x6A)
			return 1;
		return 0;
	}
}
