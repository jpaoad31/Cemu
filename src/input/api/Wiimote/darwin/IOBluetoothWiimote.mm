// macOS IOBluetooth backend for real Wii devices (Wii Remote, Wii Balance Board).
//
// Adapted from Dolphin's former macOS Wii Remote backend (dolphin-emu/dolphin,
// Source/Core/Core/HW/WiimoteReal/IOdarwin.{h,mm} and IOdarwin_private.h, removed in commit
// e9a11cdbfd "WiimoteReal: Remove WiimoteScannerDarwin"; Copyright 2016 Dolphin Emulator Project,
// GPL-2.0-or-later): the device inquiry, the baseband connection and the L2CAP control (PSM 0x11)
// and interrupt (PSM 0x13) channels opened by address without pairing, reports written to the
// interrupt channel with the 0xA2 header, input data from the channel delegate.
// Changes: the inquiry runs only on request (Dolphin's scanner ran continuously and pegged the
// CPU) and stops at the first connected device; the connection steps are asynchronous on the main
// run loop; received reports go into a thread-safe queue for Cemu's Wiimote provider.
//
// Threading: every IOBluetooth call and callback happens on the main thread (main dispatch
// queue). Cemu's reader/writer threads only touch the report queue and post writes to it.

#define BLUETOOTH_VERSION_USE_CURRENT

#ifdef IOBT_STANDALONE
#include <cstdint>
typedef uint8_t uint8;
#endif

#include "IOBluetoothWiimote.h"

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#import <CoreBluetooth/CoreBluetooth.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>

#ifdef IOBT_STANDALONE
#include <format>
#include <iostream>
#define IOBT_LOG(...) (std::cout << "[IOBluetooth] " << std::format(__VA_ARGS__) << std::endl)
#else
#define IOBT_LOG(...) cemuLog_log(LogType::InputAPI, __VA_ARGS__)
#endif

static constexpr BluetoothL2CAPPSM kPSMControl = 0x11;   // kBluetoothL2CAPPSMHIDControl
static constexpr BluetoothL2CAPPSM kPSMInterrupt = 0x13; // kBluetoothL2CAPPSMHIDInterrupt
static constexpr size_t kMaxReport = 23;                 // 0xA1 + 22 bytes
static constexpr size_t kMaxQueuedReports = 256;
static constexpr int kReportsToLog = 5;
static constexpr int kInquiryRoundSeconds = 10;          // one inquiry round, restarted until the deadline
static constexpr int kConnectTimeoutSeconds = 20;
static constexpr BluetoothHCIPageTimeout kPageTimeout = 0x2000; // * 0.625 ms = 5.1 s

static std::string NSToStd(NSString* s)
{
	if (!s)
		return {};
	const char* utf8 = [s UTF8String];
	return utf8 ? std::string(utf8) : std::string();
}

// "34-af-2c-e6-c1-87" / "34:AF:2C:E6:C1:87" -> "34af2ce6c187"
static std::string NormalizeAddress(std::string_view address)
{
	std::string out;
	for (char c : address)
	{
		if (std::isxdigit((unsigned char)c))
			out.push_back((char)std::tolower((unsigned char)c));
	}
	return out;
}

static bool IsWiiDeviceName(const std::string& name)
{
	// like Dolphin's IsValidDeviceName; "-UC" is the Wii U Pro Controller (not a Wii device)
	if (name.starts_with("Nintendo RVL-WBC"))
		return true;
	return name.starts_with("Nintendo RVL-CNT-01") && name != "Nintendo RVL-CNT-01-UC";
}

static std::string Hex(const uint8_t* data, size_t length)
{
	std::string out;
	char buf[4];
	for (size_t i = 0; i < length; i++)
	{
		snprintf(buf, sizeof(buf), i ? " %02x" : "%02x", data[i]);
		out += buf;
	}
	return out;
}

enum class ConnState
{
	Connecting,
	Connected,
	Closed,
};

@class CemuWiimoteBTConnectionDelegate;

struct IOBluetoothWiimoteConnection
{
	// main thread only
	IOBluetoothDevice* device = nil;               // retained
	IOBluetoothL2CAPChannel* control = nil;        // retained
	IOBluetoothL2CAPChannel* interrupt = nil;      // retained
	// retained and deliberately never released: IOBluetooth keeps unretained references to the
	// connection target, and a late callback after a timeout must not hit a freed object. Its
	// weak owner pointer has expired by then, so late callbacks are ignored.
	CemuWiimoteBTConnectionDelegate* delegate = nil;
	int write_errors_logged = 0;
	int reports_logged = 0;

	// set once
	std::string name;
	std::string address; // as macOS prints it, "34-af-2c-e6-c1-87"
	std::string address_key;

	std::atomic<ConnState> state{ConnState::Connecting};

	std::mutex queue_mutex;
	std::deque<std::vector<uint8>> queue;
	bool queue_overflow_logged = false;
};
using ConnectionPtr = std::shared_ptr<IOBluetoothWiimoteConnection>;

// registry: connecting and connected devices
struct RegistryEntry
{
	ConnectionPtr connection;
	std::shared_ptr<IOBluetoothWiimote> device; // set once connected
};
static std::mutex s_registry_mutex;
static std::vector<RegistryEntry> s_registry;

// scan state (main thread only, except the status copy)
static IOBluetoothDeviceInquiry* s_inquiry = nil;
static id s_inquiry_delegate = nil;
static CBCentralManager* s_cb_manager = nil; // only created to trigger the Bluetooth permission prompt
static uint32_t s_scan_generation = 0;
static bool s_scan_active = false;
static bool s_scan_cancel = false;
static bool s_inquiry_running = false;
static int s_connect_attempts_pending = 0;
static std::chrono::steady_clock::time_point s_scan_deadline;
static std::set<std::string> s_scan_logged_addresses;
static std::vector<IOBluetoothDevice*> s_scan_candidates; // retained

static std::mutex s_status_mutex;
static std::string s_status_text;
static std::atomic_bool s_status_scanning = false;
static std::atomic<int64_t> s_status_deadline_ms = 0;

static void SetStatus(const std::string& text)
{
	std::scoped_lock lock(s_status_mutex);
	s_status_text = text;
}

static int64_t SteadyMs(std::chrono::steady_clock::time_point tp)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

static void RunOnMain(dispatch_block_t block)
{
	if ([NSThread isMainThread])
		block();
	else
		dispatch_async(dispatch_get_main_queue(), block);
}

static ConnectionPtr FindConnection(const std::string& address_key)
{
	std::scoped_lock lock(s_registry_mutex);
	for (auto& entry : s_registry)
	{
		if (entry.connection->address_key == address_key)
			return entry.connection;
	}
	return {};
}

static void RemoveConnection(const ConnectionPtr& connection)
{
	std::scoped_lock lock(s_registry_mutex);
	std::erase_if(s_registry, [&](const RegistryEntry& e) { return e.connection == connection; });
}

static void OnConnectAttemptDone(const ConnectionPtr& connection, bool success);
static void CloseConnection(const ConnectionPtr& connection, const char* reason);
static void RestartInquiryOrFinish();

// ---------------------------------------------------------------------------------------------
// connection delegate (baseband + both L2CAP channels)

@interface CemuWiimoteBTConnectionDelegate : NSObject <IOBluetoothL2CAPChannelDelegate>
{
@public
	std::weak_ptr<IOBluetoothWiimoteConnection>* owner;
}
@end

@implementation CemuWiimoteBTConnectionDelegate
- (void)dealloc
{
	delete owner;
	owner = nullptr;
	[super dealloc];
}

- (ConnectionPtr)connection
{
	return owner ? owner->lock() : ConnectionPtr();
}

// baseband (ACL) connection, from openConnection:withPageTimeout:authenticationRequired:
- (void)connectionComplete:(IOBluetoothDevice*)device status:(IOReturn)status
{
	auto connection = [self connection];
	if (!connection || connection->state != ConnState::Connecting)
		return;
	if (status != kIOReturnSuccess)
	{
		IOBT_LOG("IOBluetooth: {} baseband connection failed: IOReturn 0x{:08x}", connection->address, (uint32_t)status);
		CloseConnection(connection, nullptr);
		OnConnectAttemptDone(connection, false);
		return;
	}
	IOBT_LOG("IOBluetooth: {} baseband connection open (0x{:08x}), opening L2CAP PSM 0x11 (control)", connection->address, (uint32_t)status);
	IOBluetoothL2CAPChannel* channel = nil;
	const IOReturn ret = [device openL2CAPChannelAsync:&channel withPSM:kPSMControl delegate:self];
	if (ret != kIOReturnSuccess)
	{
		IOBT_LOG("IOBluetooth: {} L2CAP PSM 0x11 open request failed: IOReturn 0x{:08x}", connection->address, (uint32_t)ret);
		CloseConnection(connection, nullptr);
		OnConnectAttemptDone(connection, false);
		return;
	}
	// retained explicitly, as Dolphin did (the channels were over-released otherwise)
	connection->control = [channel retain];
}

- (void)l2capChannelOpenComplete:(IOBluetoothL2CAPChannel*)channel status:(IOReturn)status
{
	auto connection = [self connection];
	if (!connection || connection->state != ConnState::Connecting)
		return;
	const unsigned psm = [channel PSM];
	if (status != kIOReturnSuccess)
	{
		IOBT_LOG("IOBluetooth: {} L2CAP PSM 0x{:02x} failed to open: IOReturn 0x{:08x}", connection->address, psm, (uint32_t)status);
		CloseConnection(connection, nullptr);
		OnConnectAttemptDone(connection, false);
		return;
	}
	if (psm == kPSMControl)
	{
		IOBT_LOG("IOBluetooth: {} L2CAP PSM 0x11 (control) open, opening PSM 0x13 (interrupt)", connection->address);
		IOBluetoothL2CAPChannel* interrupt = nil;
		const IOReturn ret = [connection->device openL2CAPChannelAsync:&interrupt withPSM:kPSMInterrupt delegate:self];
		if (ret != kIOReturnSuccess)
		{
			IOBT_LOG("IOBluetooth: {} L2CAP PSM 0x13 open request failed: IOReturn 0x{:08x}", connection->address, (uint32_t)ret);
			CloseConnection(connection, nullptr);
			OnConnectAttemptDone(connection, false);
			return;
		}
		connection->interrupt = [interrupt retain];
	}
	else if (psm == kPSMInterrupt)
	{
		IOBT_LOG("IOBluetooth: {} L2CAP PSM 0x13 (interrupt) open: connected to \"{}\"", connection->address, connection->name);
		connection->state = ConnState::Connected;
		{
			std::scoped_lock lock(s_registry_mutex);
			for (auto& entry : s_registry)
			{
				if (entry.connection == connection)
					entry.device = std::make_shared<IOBluetoothWiimote>(connection);
			}
		}
		OnConnectAttemptDone(connection, true);
	}
}

- (void)l2capChannelData:(IOBluetoothL2CAPChannel*)channel data:(void*)dataPointer length:(size_t)length
{
	auto connection = [self connection];
	if (!connection)
		return;
	const auto* data = (const uint8_t*)dataPointer;
	if ([channel PSM] != kPSMInterrupt)
	{
		IOBT_LOG("IOBluetooth: {} data on the control channel ({} bytes): {}", connection->address, length, Hex(data, std::min<size_t>(length, kMaxReport)));
		return;
	}
	if (connection->reports_logged < kReportsToLog)
	{
		connection->reports_logged++;
		IOBT_LOG("IOBluetooth: {} report #{} ({} bytes): {}", connection->address, connection->reports_logged, length, Hex(data, std::min<size_t>(length, kMaxReport)));
	}
	// input reports start with 0xA1 (HID DATA | input)
	if (length < 2 || length > kMaxReport || data[0] != 0xA1)
		return;
	std::scoped_lock lock(connection->queue_mutex);
	if (connection->queue.size() >= kMaxQueuedReports)
	{
		connection->queue.pop_front();
		if (!connection->queue_overflow_logged)
		{
			connection->queue_overflow_logged = true;
			IOBT_LOG("IOBluetooth: {} report queue full, dropping old reports", connection->address);
		}
	}
	connection->queue.emplace_back(data + 1, data + length);
}

- (void)l2capChannelWriteComplete:(IOBluetoothL2CAPChannel*)channel refcon:(void*)refcon status:(IOReturn)status
{
	free(refcon);
	if (status == kIOReturnSuccess)
		return;
	auto connection = [self connection];
	if (connection && connection->write_errors_logged < 5)
	{
		connection->write_errors_logged++;
		IOBT_LOG("IOBluetooth: {} write failed: IOReturn 0x{:08x}", connection->address, (uint32_t)status);
	}
}

- (void)l2capChannelClosed:(IOBluetoothL2CAPChannel*)channel
{
	auto connection = [self connection];
	if (!connection || connection->state == ConnState::Closed)
		return;
	const bool was_connecting = connection->state == ConnState::Connecting;
	char reason[64];
	snprintf(reason, sizeof(reason), "the device closed L2CAP PSM 0x%02x", (unsigned)[channel PSM]);
	CloseConnection(connection, reason);
	if (was_connecting)
		OnConnectAttemptDone(connection, false);
}

- (void)l2capChannelReconfigured:(IOBluetoothL2CAPChannel*)channel {}
- (void)l2capChannelQueueSpaceAvailable:(IOBluetoothL2CAPChannel*)channel {}
@end

// ---------------------------------------------------------------------------------------------
// connect / close (main thread)

static void CloseConnection(const ConnectionPtr& connection, const char* reason)
{
	const ConnState previous = connection->state.exchange(ConnState::Closed);
	if (previous == ConnState::Closed)
		return;
	if (reason)
		IOBT_LOG("IOBluetooth: {} \"{}\" disconnected: {}", connection->address, connection->name, reason);
	if (connection->interrupt)
	{
		[connection->interrupt setDelegate:nil];
		[connection->interrupt closeChannel];
		[connection->interrupt release];
		connection->interrupt = nil;
	}
	if (connection->control)
	{
		[connection->control setDelegate:nil];
		[connection->control closeChannel];
		[connection->control release];
		connection->control = nil;
	}
	if (connection->device)
	{
		const IOReturn ret = [connection->device closeConnection];
		if (reason)
			IOBT_LOG("IOBluetooth: {} baseband connection closed: IOReturn 0x{:08x}", connection->address, (uint32_t)ret);
		[connection->device release];
		connection->device = nil;
	}
	RemoveConnection(connection);
	// the delegate stays alive until the connection object goes (pending callbacks find it closed)
}

static void ConnectDevice(IOBluetoothDevice* device)
{
	const std::string address = NSToStd([device addressString]);
	const std::string key = NormalizeAddress(address);
	if (key.empty() || FindConnection(key))
		return;

	auto connection = std::make_shared<IOBluetoothWiimoteConnection>();
	connection->device = [device retain];
	connection->name = NSToStd([device name]);
	connection->address = address;
	connection->address_key = key;
	connection->delegate = [[CemuWiimoteBTConnectionDelegate alloc] init];
	connection->delegate->owner = new std::weak_ptr<IOBluetoothWiimoteConnection>(connection);
	{
		std::scoped_lock lock(s_registry_mutex);
		s_registry.push_back({connection, nullptr});
	}
	s_connect_attempts_pending++;
	SetStatus("Connecting to " + connection->name + " (" + address + ")...");

	if ([device isConnected])
	{
		// already has a baseband link (e.g. it just reconnected): go straight to the channels
		IOBT_LOG("IOBluetooth: {} \"{}\" already has a baseband connection, opening L2CAP", address, connection->name);
		[connection->delegate connectionComplete:device status:kIOReturnSuccess];
	}
	else
	{
		IOBT_LOG("IOBluetooth: {} \"{}\": opening the baseband connection (no pairing, no authentication)", address, connection->name);
		const IOReturn ret = [device openConnection:connection->delegate withPageTimeout:kPageTimeout authenticationRequired:NO];
		if (ret != kIOReturnSuccess)
		{
			IOBT_LOG("IOBluetooth: {} baseband connection request failed: IOReturn 0x{:08x}", address, (uint32_t)ret);
			CloseConnection(connection, nullptr);
			OnConnectAttemptDone(connection, false);
			return;
		}
	}

	std::weak_ptr<IOBluetoothWiimoteConnection> weak = connection;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)kConnectTimeoutSeconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		auto c = weak.lock();
		if (c && c->state == ConnState::Connecting)
		{
			IOBT_LOG("IOBluetooth: {} connection timed out after {} s", c->address, kConnectTimeoutSeconds);
			CloseConnection(c, nullptr);
			OnConnectAttemptDone(c, false);
		}
	});
}

// ---------------------------------------------------------------------------------------------
// scan (main thread)

static void FinishScan(const std::string& result)
{
	if (!s_scan_active)
		return;
	s_scan_active = false;
	s_status_scanning = false;
	if (s_inquiry)
	{
		[s_inquiry setDelegate:nil];
		if (s_inquiry_running)
			[s_inquiry stop];
		[s_inquiry release];
		s_inquiry = nil;
	}
	s_inquiry_running = false;
	for (IOBluetoothDevice* d : s_scan_candidates)
		[d release];
	s_scan_candidates.clear();
	IOBT_LOG("IOBluetooth: scan ended: {}", result);
	SetStatus(result);
}

static bool HasConnectedDevice()
{
	std::scoped_lock lock(s_registry_mutex);
	return std::any_of(s_registry.begin(), s_registry.end(), [](const RegistryEntry& e) { return e.connection->state == ConnState::Connected; });
}

static void OnConnectAttemptDone(const ConnectionPtr& connection, bool success)
{
	if (s_connect_attempts_pending > 0)
		s_connect_attempts_pending--;
	if (success)
		SetStatus("Connected: " + connection->name + " (" + connection->address + ")");
	else
		SetStatus("Could not connect to " + connection->name + " (" + connection->address + "), see the log");
	if (!s_scan_active || s_connect_attempts_pending > 0)
		return;
	if (success)
		FinishScan("connected " + connection->name + " (" + connection->address + ")");
	else
		RestartInquiryOrFinish();
}

static void ConsiderDevice(IOBluetoothDevice* device)
{
	const std::string name = NSToStd([device name]);
	const std::string address = NSToStd([device addressString]);
	const std::string key = NormalizeAddress(address);
	const std::string logKey = key + "|" + name;
	if (!s_scan_logged_addresses.contains(logKey))
	{
		s_scan_logged_addresses.insert(logKey);
		IOBT_LOG("IOBluetooth: found device {} \"{}\" (class of device 0x{:06x}){}", address, name.empty() ? "<name not known yet>" : name,
			(uint32_t)[device classOfDevice], IsWiiDeviceName(name) ? ": a Nintendo RVL device" : "");
	}
	if (!IsWiiDeviceName(name) || FindConnection(key))
		return;
	for (IOBluetoothDevice* d : s_scan_candidates)
	{
		if ([d isEqual:device])
			return;
	}
	s_scan_candidates.push_back([device retain]);
	SetStatus("Found " + name + " (" + address + "), connecting...");
	// paging during an inquiry is unreliable: stop it, connect in deviceInquiryComplete
	if (s_inquiry_running)
		[s_inquiry stop];
}

static void StartInquiryRound()
{
	const auto now = std::chrono::steady_clock::now();
	const int remaining = (int)std::chrono::duration_cast<std::chrono::seconds>(s_scan_deadline - now).count();
	if (remaining < 2)
	{
		FinishScan("no Nintendo RVL device found");
		return;
	}
	if (!s_inquiry)
	{
		s_inquiry = [[IOBluetoothDeviceInquiry alloc] initWithDelegate:s_inquiry_delegate];
		[s_inquiry setUpdateNewDeviceNames:YES];
	}
	else
		[s_inquiry clearFoundDevices];
	[s_inquiry setInquiryLength:(uint8_t)std::min(remaining, kInquiryRoundSeconds)];
	const IOReturn ret = [s_inquiry start];
	if (ret != kIOReturnSuccess)
	{
		IOBT_LOG("IOBluetooth: device inquiry failed to start: IOReturn 0x{:08x} (Bluetooth off, or access denied?)", (uint32_t)ret);
		FinishScan("the Bluetooth device inquiry failed to start, see the log");
		return;
	}
	s_inquiry_running = true;
}

static void RestartInquiryOrFinish()
{
	if (!s_scan_active)
		return;
	if (s_scan_cancel)
	{
		FinishScan("cancelled");
		return;
	}
	if (std::chrono::steady_clock::now() >= s_scan_deadline)
	{
		FinishScan("no Nintendo RVL device found");
		return;
	}
	SetStatus("Scanning: press the red sync button on the board (or 1+2 on a Wii Remote)");
	StartInquiryRound();
}

@interface CemuWiimoteBTInquiryDelegate : NSObject <IOBluetoothDeviceInquiryDelegate>
@end

@implementation CemuWiimoteBTInquiryDelegate
- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry*)sender device:(IOBluetoothDevice*)device
{
	if (sender == s_inquiry)
		ConsiderDevice(device);
}

- (void)deviceInquiryDeviceNameUpdated:(IOBluetoothDeviceInquiry*)sender device:(IOBluetoothDevice*)device devicesRemaining:(uint32_t)devicesRemaining
{
	if (sender == s_inquiry)
		ConsiderDevice(device);
}

- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry*)sender error:(IOReturn)error aborted:(BOOL)aborted
{
	if (sender != s_inquiry || !s_scan_active)
		return;
	s_inquiry_running = false;
	if (error != kIOReturnSuccess)
		IOBT_LOG("IOBluetooth: inquiry round ended with IOReturn 0x{:08x}{}", (uint32_t)error, aborted ? " (stopped)" : "");
	if (s_scan_cancel)
	{
		FinishScan("cancelled");
		return;
	}
	if (!s_scan_candidates.empty())
	{
		auto candidates = std::move(s_scan_candidates);
		s_scan_candidates.clear();
		for (IOBluetoothDevice* d : candidates)
		{
			ConnectDevice(d);
			[d release];
		}
		if (s_connect_attempts_pending > 0)
			return; // continues in OnConnectAttemptDone
	}
	RestartInquiryOrFinish();
}
@end

static const char* AuthorizationName(CBManagerAuthorization a)
{
	switch (a)
	{
	case CBManagerAuthorizationNotDetermined: return "not determined";
	case CBManagerAuthorizationRestricted: return "restricted";
	case CBManagerAuthorizationDenied: return "denied";
	case CBManagerAuthorizationAllowedAlways: return "allowed";
	}
	return "unknown";
}

// false if Bluetooth can't be used (logged)
static bool CheckBluetoothAccess()
{
	const CBManagerAuthorization auth = [CBManager authorization];
	if (auth == CBManagerAuthorizationDenied || auth == CBManagerAuthorizationRestricted)
	{
		IOBT_LOG("IOBluetooth: Bluetooth access is {} for this app. Allow it in System Settings > Privacy & Security > Bluetooth, then try again.", AuthorizationName(auth));
		SetStatus("Bluetooth access denied: allow Cemu in System Settings > Privacy & Security > Bluetooth");
		return false;
	}
	if (auth == CBManagerAuthorizationNotDetermined && !s_cb_manager)
	{
		// asks the user (needs NSBluetoothAlwaysUsageDescription in Info.plist)
		IOBT_LOG("IOBluetooth: Bluetooth access not determined yet, asking for it");
		s_cb_manager = [[CBCentralManager alloc] initWithDelegate:nil queue:dispatch_get_main_queue()];
	}
	IOBluetoothHostController* host = [IOBluetoothHostController defaultController];
	if (!host || [host powerState] != kBluetoothHCIPowerStateON)
	{
		IOBT_LOG("IOBluetooth: no Bluetooth controller, or Bluetooth is off (authorization: {})", AuthorizationName(auth));
		SetStatus("Bluetooth is off or not available");
		return false;
	}
	IOBT_LOG("IOBluetooth: host controller {}, access {}", NSToStd([host addressAsString]), AuthorizationName(auth));
	return true;
}

// ---------------------------------------------------------------------------------------------
// public interface

IOBluetoothWiimote::IOBluetoothWiimote(std::shared_ptr<IOBluetoothWiimoteConnection> connection)
	: m_connection(std::move(connection))
{
}

IOBluetoothWiimote::~IOBluetoothWiimote() = default;

bool IOBluetoothWiimote::write_data(const std::vector<uint8>& data)
{
	if (m_connection->state != ConnState::Connected || data.empty() || data.size() >= kMaxReport)
		return false;
	// output reports start with 0xA2 (HID DATA | output), on the interrupt channel like Dolphin
	const size_t length = data.size() + 1;
	auto* buffer = (uint8_t*)malloc(length);
	buffer[0] = 0xA2;
	memcpy(buffer + 1, data.data(), data.size());
	auto connection = m_connection;
	dispatch_async(dispatch_get_main_queue(), ^{
		if (connection->state != ConnState::Connected || !connection->interrupt)
		{
			free(buffer);
			return;
		}
		const IOReturn ret = [connection->interrupt writeAsync:buffer length:(UInt16)length refcon:buffer];
		if (ret != kIOReturnSuccess)
		{
			free(buffer);
			if (connection->write_errors_logged < 5)
			{
				connection->write_errors_logged++;
				IOBT_LOG("IOBluetooth: {} writeAsync failed: IOReturn 0x{:08x}", connection->address, (uint32_t)ret);
			}
		}
	});
	return true;
}

std::optional<std::vector<uint8>> IOBluetoothWiimote::read_data()
{
	std::scoped_lock lock(m_connection->queue_mutex);
	if (!m_connection->queue.empty())
	{
		auto report = std::move(m_connection->queue.front());
		m_connection->queue.pop_front();
		return report;
	}
	if (m_connection->state != ConnState::Connected)
		return std::nullopt;
	return std::vector<uint8>{};
}

bool IOBluetoothWiimote::operator==(const WiimoteDevice& o) const
{
	auto other = dynamic_cast<const IOBluetoothWiimote*>(&o);
	return other && other->m_connection->address_key == m_connection->address_key;
}

std::string IOBluetoothWiimote::get_product_name() const
{
	return m_connection->name;
}

std::string IOBluetoothWiimote::get_location() const
{
	return m_connection->address + " (IOBluetooth)";
}

std::string IOBluetoothWiimote::get_address() const
{
	return m_connection->address;
}

std::vector<WiimoteDevicePtr> IOBluetoothWiimote::get_devices()
{
	std::vector<WiimoteDevicePtr> result;
	std::scoped_lock lock(s_registry_mutex);
	for (auto& entry : s_registry)
	{
		if (entry.device && entry.connection->state == ConnState::Connected)
			result.push_back(entry.device);
	}
	return result;
}

bool IOBluetoothWiimote::is_handling_address(std::string_view address)
{
	const auto key = NormalizeAddress(address);
	return !key.empty() && FindConnection(key) != nullptr;
}

void IOBluetoothWiimote::start_scan(int timeout_seconds)
{
	timeout_seconds = std::clamp(timeout_seconds, 2, 600);
	RunOnMain(^{
		if (s_scan_active)
			return;
		if (!CheckBluetoothAccess())
			return;
		if (!s_inquiry_delegate)
			s_inquiry_delegate = [[CemuWiimoteBTInquiryDelegate alloc] init];
		s_scan_generation++;
		s_scan_active = true;
		s_scan_cancel = false;
		s_scan_logged_addresses.clear();
		s_scan_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
		s_status_deadline_ms = SteadyMs(s_scan_deadline);
		s_status_scanning = true;
		IOBT_LOG("IOBluetooth: scan started (up to {} s, ends at the first Nintendo RVL device connected)", timeout_seconds);
		SetStatus("Scanning: press the red sync button on the board (or 1+2 on a Wii Remote)");
		StartInquiryRound();
		// the deadline also stops a running round (rounds are at most kInquiryRoundSeconds anyway)
		const uint32_t generation = s_scan_generation;
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_seconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			if (s_scan_active && s_scan_generation == generation && s_inquiry_running && s_connect_attempts_pending == 0)
				[s_inquiry stop];
		});
	});
}

void IOBluetoothWiimote::cancel_scan()
{
	RunOnMain(^{
		if (!s_scan_active)
			return;
		IOBT_LOG("IOBluetooth: scan cancelled by the user");
		s_scan_cancel = true;
		if (s_connect_attempts_pending > 0)
			return; // finishes after the connection attempt
		FinishScan("cancelled");
	});
}

void IOBluetoothWiimote::connect_address(const std::string& address)
{
	const std::string addr = address;
	RunOnMain(^{
		if (!CheckBluetoothAccess())
			return;
		IOBluetoothDevice* device = [IOBluetoothDevice deviceWithAddressString:[NSString stringWithUTF8String:addr.c_str()]];
		if (!device)
		{
			IOBT_LOG("IOBluetooth: invalid address \"{}\"", addr);
			return;
		}
		IOBT_LOG("IOBluetooth: connecting to {} by address (name known to macOS: \"{}\")", addr, NSToStd([device name]));
		ConnectDevice(device);
	});
}

void IOBluetoothWiimote::disconnect_all()
{
	RunOnMain(^{
		std::vector<ConnectionPtr> connections;
		{
			std::scoped_lock lock(s_registry_mutex);
			for (auto& entry : s_registry)
				connections.push_back(entry.connection);
		}
		for (auto& c : connections)
			CloseConnection(c, "closed by Cemu");
	});
}

IOBluetoothWiimote::ScanStatus IOBluetoothWiimote::get_status()
{
	ScanStatus status;
	status.scanning = s_status_scanning;
	if (status.scanning)
	{
		const int64_t left = s_status_deadline_ms - SteadyMs(std::chrono::steady_clock::now());
		status.seconds_left = (int)std::max<int64_t>(0, (left + 999) / 1000);
	}
	{
		std::scoped_lock lock(s_status_mutex);
		status.text = s_status_text;
	}
	std::scoped_lock lock(s_registry_mutex);
	for (auto& entry : s_registry)
	{
		if (entry.connection->state == ConnState::Connected)
			status.connected.push_back(entry.connection->name + " (" + entry.connection->address + ")");
	}
	return status;
}
