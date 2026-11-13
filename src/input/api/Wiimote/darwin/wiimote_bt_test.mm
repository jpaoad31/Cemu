// Standalone test for the IOBluetooth Wii device backend (IOBluetoothWiimote.mm), outside Cemu.
//
//   wiimote_bt_test scan [seconds]     scan (default 60 s, ends at the first Nintendo RVL device
//                                      connected), then talk to it
//   wiimote_bt_test connect <address>  connect by address (device in sync mode), then talk to it
//
// After connecting: LED 1 on, a status request, then the extension ID read (0xA400FA, 6 bytes;
// a Balance Board answers 00 00 a4 20 04 02); prints every report for 5 s, then disconnects.

#include <cstdint>
typedef uint8_t uint8;

#include "IOBluetoothWiimote.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static void PrintReport(const std::vector<uint8>& r)
{
	std::string hex;
	char buf[4];
	for (uint8 b : r)
	{
		snprintf(buf, sizeof(buf), "%02x ", b);
		hex += buf;
	}
	const char* what = "";
	if (!r.empty())
	{
		switch (r[0])
		{
		case 0x20: what = " (status: buttons, flags (0x02 = extension), 0 0, battery)"; break;
		case 0x21: what = " (memory read reply)"; break;
		case 0x22: what = " (acknowledge)"; break;
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x3d: what = " (data report)"; break;
		}
	}
	printf("  report: %s%s\n", hex.c_str(), what);
	fflush(stdout);
}

static void Talk()
{
	// wait for a connection (the scan ends on its own)
	WiimoteDevicePtr device;
	int last_left = -1;
	while (!device)
	{
		auto devices = IOBluetoothWiimote::get_devices();
		if (!devices.empty())
		{
			device = devices.front();
			break;
		}
		const auto status = IOBluetoothWiimote::get_status();
		if (!status.scanning && status.connected.empty() && status.text.find("Connecting") == std::string::npos && status.text.find("connecting") == std::string::npos
			&& !status.text.empty() && status.text.find("Scanning") == std::string::npos)
		{
			printf("no device: %s\n", status.text.c_str());
			exit(1);
		}
		if (status.scanning && status.seconds_left != last_left && status.seconds_left % 5 == 0)
		{
			last_left = status.seconds_left;
			printf("scanning, %d s left: %s\n", status.seconds_left, status.text.c_str());
			fflush(stdout);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	printf("connected: \"%s\" at %s\n", device->get_product_name().c_str(), device->get_location().c_str());
	device->write_data({0x11, 0x10});                               // LED 1
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	device->write_data({0x15, 0x00});                               // status request
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	device->write_data({0x16, 0x04, 0xA4, 0x00, 0xF0, 0x01, 0x55}); // extension init, unencrypted
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	device->write_data({0x16, 0x04, 0xA4, 0x00, 0xFB, 0x01, 0x00});
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	device->write_data({0x17, 0x04, 0xA4, 0x00, 0xFA, 0x00, 0x06}); // read extension ID
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	bool requested_data = false;
	while (std::chrono::steady_clock::now() < end)
	{
		auto r = device->read_data();
		if (!r)
		{
			printf("device disconnected\n");
			exit(1);
		}
		if (r->empty())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}
		PrintReport(*r);
		if (!requested_data && (*r)[0] == 0x21)
		{
			requested_data = true;
			device->write_data({0x12, 0x00, 0x34}); // report mode 0x34 (buttons + 19 extension bytes)
		}
	}
	printf("done, disconnecting\n");
	IOBluetoothWiimote::disconnect_all();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	exit(0);
}

int main(int argc, char** argv)
{
	@autoreleasepool
	{
		if (argc >= 2 && strcmp(argv[1], "scan") == 0)
		{
			const int seconds = argc >= 3 ? atoi(argv[2]) : 60;
			printf("press the red sync button on the board (or 1+2 on a Wii Remote); scanning up to %d s\n", seconds);
			IOBluetoothWiimote::start_scan(seconds);
		}
		else if (argc >= 3 && strcmp(argv[1], "connect") == 0)
			IOBluetoothWiimote::connect_address(argv[2]);
		else
		{
			fprintf(stderr, "usage: %s scan [seconds] | connect <address, e.g. 34-af-2c-e6-c1-87>\n", argv[0]);
			return 2;
		}
		std::thread(Talk).detach();
		// IOBluetooth runs on the main run loop
		CFRunLoopRun();
	}
	return 0;
}
