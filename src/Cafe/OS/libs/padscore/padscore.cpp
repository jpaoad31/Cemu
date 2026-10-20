#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "WindowSystem.h"
#include "input/InputManager.h"
#include "input/emulated/BalanceBoardController.h"
#include "Cafe/CafeSystem.h"
#include "config/CemuConfig.h"

// KPAD

enum class KPAD_ERROR : sint32
{
	NONE = 0,
	NO_SAMPLE_DATA = -1,
	NO_CONTROLLER = -2,
	NOT_INITIALIZED = -5,
};

// WPAD (Wii Controller stuff)

enum WPADRumble
{
	kStopRumble = 0,
	kStartRumble = 1,
};

enum class WPADBatteryLevel : uint8
{
	CRITICAL = 0,
	LOW = 1,
	MEDIUM = 2,
	HIGH = 3,
	FULL = 4,
};

enum class WPADLed : uint8
{
	CHAN0 = (1 << 0),
	CHAN1 = (1 << 1),
	CHAN2 = (1 << 2),
	CHAN3 = (1 << 3),
};

namespace padscore 
{
	enum WPADState_t
	{
		kWPADStateMaster = 0,
		kWPADStateShutdown,
		kWPADStateInitializing,
		kWPADStateAcquired,
		kWPADStateReleased,
	};
	KPADUnifiedWpadStatus_t* g_kpad_ringbuffer = nullptr;
	uint32 g_kpad_ringbuffer_length = 0;
	bool g_wpad_callback_by_kpad = false;

	WPADState_t g_wpad_state = kWPADStateMaster;

	struct {
		SysAllocator<coreinit::OSAlarm_t> alarm;
		bool kpad_initialized = false;

		struct WPADData
		{
			MEMPTR<void> extension_callback;
			MEMPTR<void> connectCallback;
			MEMPTR<void> sampling_callback;
			MEMPTR<void> dpd_callback;
			bool dpd_enabled = true;

			bool disconnectCalled = false;

			BtnRepeat btn_repeat{};
		} controller_data[InputManager::kMaxWPADControllers] = {};

		int max_controllers = kWPADMaxControllers; // max bt controllers?
	} g_padscore;

	void KPADReadBalanceBoard(WPADController& controller, KPADStatus_t& status);
}


#pragma region WPAD

void padscoreExport_WPADGetStatus(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetStatus()");
	uint32 status = 1;
	osLib_returnFromFunction(hCPU, status);
}

void padscoreExport_WPADGetBatteryLevel(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADGetBatteryLevel()");
	osLib_returnFromFunction(hCPU, (uint32)WPADBatteryLevel::FULL);
}

void padscoreExport_WPADProbe(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(type, uint32be, 1);

	cemuLog_log(LogType::InputAPI, "WPADProbe({})", channel);

	if(const auto controller = InputManager::instance().get_wpad_controller(channel))
	{
		if(type)
			*type = controller->get_device_type();

		osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
	}
	else
	{
		if(type)
			*type = 253;

		osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
	}
}

typedef struct
{
	uint32be dpd;
	uint32be speaker;
	uint32be attach;
	uint32be lowBat;
	uint32be nearempty;
	betype<WPADBatteryLevel> batteryLevel;
	betype<WPADLed> led;
	uint8 protocol;
	uint8 firmware;
}WPADInfo_t;

static_assert(sizeof(WPADInfo_t) == 0x18); // unsure

static void WPADFillInfo(uint32 channel, WPADController& controller, WPADInfo_t* wpadInfo)
{
	wpadInfo->dpd = FALSE;
	wpadInfo->speaker = FALSE;
	wpadInfo->attach = FALSE;
	wpadInfo->lowBat = FALSE;
	wpadInfo->nearempty = FALSE;
	wpadInfo->batteryLevel = WPADBatteryLevel::FULL;
	wpadInfo->led = WPADLed::CHAN0;

	if (const auto board = dynamic_cast<BalanceBoardController*>(&controller))
	{
		// the board is a remote with its load cells as an extension, lit on its own channel
		const int level = board->get_battery_level();
		wpadInfo->attach = TRUE;
		wpadInfo->batteryLevel = (WPADBatteryLevel)level;
		wpadInfo->lowBat = level <= 1;
		wpadInfo->nearempty = level == 0;
		wpadInfo->led = (WPADLed)(1 << (channel & 3));
	}
}

void padscoreExport_WPADGetInfoAsync(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	ppcDefineParamMPTR(callbackFunc, 2);
	cemuLog_log(LogType::InputAPI, "WPADGetInfoAsync({}, 0x{:08x}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo), callbackFunc);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			WPADFillInfo(channel, *controller, wpadInfo);

			if(callbackFunc != MPTR_NULL)
				coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NONE);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	if (callbackFunc != MPTR_NULL)
		coreinitAsyncCallback_add(callbackFunc, 2, channel, (uint32)KPAD_ERROR::NO_CONTROLLER);

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(wpadStatus, WPADStatus_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADRead({}, {:x})", channel, memory_getVirtualOffsetFromPointer(wpadStatus));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if(const auto controller = InputManager::instance().get_wpad_controller(channel) )
		{
			controller->WPADRead(wpadStatus);
		}
	}
	else
	{
		debugBreakpoint();
	}
	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(fmt, 1);
	cemuLog_log(LogType::InputAPI, "WPADSetDataFormat({}, {})", channel, fmt);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			controller->set_data_format((WPADDataFormat)fmt);
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_WPADGetDataFormat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	cemuLog_log(LogType::InputAPI, "WPADGetDataFormat({})", channel);

	sint32 dataFormat = kDataFormat_CORE;
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
			dataFormat = controller->get_data_format();
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, dataFormat);
}

void padscoreExport_WPADGetInfo(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamStructPtr(wpadInfo, WPADInfo_t, 1);
	cemuLog_log(LogType::InputAPI, "WPADGetInfo({}, 0x{:08x})", channel, memory_getVirtualOffsetFromPointer(wpadInfo));

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			WPADFillInfo(channel, *controller, wpadInfo);

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}


void padscoreExport_WPADIsMotorEnabled(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::InputAPI, "WPADIsMotorEnabled()");
	osLib_returnFromFunction(hCPU, TRUE);
}

void padscoreExport_WPADControlMotor(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	cemuLog_log(LogType::InputAPI, "WPADControlMotor({}, {})", channel, command);
	
	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if( command == kStartRumble )
				controller->start_rumble();
			else
				controller->stop_rumble();
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}
#pragma endregion



#pragma region KPAD
void padscoreExport_KPADGetUnifiedWpadStatus(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(status, KPADUnifiedWpadStatus_t, 1);
	ppcDefineParamU32(count, 2);

	cemuLog_log(LogType::InputAPI, "KPADGetUnifiedWpadStatus({}, 0x{:08x}, 0x{:x})", channel, MEMPTR<void>(status).GetMPTR(), count);

	if (channel < InputManager::kMaxWPADControllers)
	{
		memset(status, 0x00, sizeof(KPADUnifiedWpadStatus_t) * count);

		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			switch (controller->get_data_format())
			{
			case WPAD_FMT_CORE:
			case WPAD_FMT_CORE_ACC:
			case WPAD_FMT_CORE_ACC_DPD:
			case WPAD_FMT_BALANCE_CHECKER:
			{
				status->fmt = controller->get_data_format();
				controller->WPADRead(&status->u.core);
				break;
			}
			default:
				debugBreakpoint();
			}
		}
		else
		{
			status->u.core.err = WPAD_ERR_NO_CONTROLLER;
		}
	}
	else
	{
		debugBreakpoint();
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetBtnRepeat(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	float delaySec = hCPU->fpr[1].fpr;
	float pulseSec = hCPU->fpr[2].fpr;
	cemuLog_log(LogType::InputAPI, "KPADSetBtnRepeat({}, {}, {})", channel, delaySec, pulseSec);

	if (channel < InputManager::kMaxWPADControllers)
	{
		padscore::g_padscore.controller_data[channel].btn_repeat = { (int)delaySec, (int)pulseSec };
	}

	osLib_returnFromFunction(hCPU, 0);
}

void padscoreExport_KPADSetSamplingCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetSamplingCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].sampling_callback;
	padscore::g_padscore.controller_data[channel].sampling_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_WPADControlDpd(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamU32(command, 1);
	ppcDefineParamMPTR(callback, 2);

	cemuLog_log(LogType::InputAPI, "WPADControlDpd({}, {}, 0x{:x})", channel, command, callback);

	if (channel < InputManager::kMaxWPADControllers)
	{
		if (const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			padscore::g_padscore.controller_data[channel].dpd_callback = callback;
			if (callback)
			{
				coreinitAsyncCallback_add(callback, 2, channel, WPAD_ERR_NONE);
			}

			osLib_returnFromFunction(hCPU, WPAD_ERR_NONE);
			return;
		}
	}

	osLib_returnFromFunction(hCPU, WPAD_ERR_NO_CONTROLLER);
}

void padscoreExport_WPADSetExtensionCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "WPADSetExtensionCallback({}, 0x{:x})", channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].extension_callback;
	padscore::g_padscore.controller_data[channel].extension_callback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

void padscoreExport_KPADSetConnectCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamMPTR(callback, 1);

	cemuLog_log(LogType::InputAPI, "KPADSetConnectCallback({}, 0x{:x})",channel, callback);

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		osLib_returnFromFunction(hCPU, MPTR_NULL);
		return;
	}

	const auto old_callback = padscore::g_padscore.controller_data[channel].connectCallback;
	padscore::g_padscore.controller_data[channel].connectCallback = callback;
	osLib_returnFromFunction(hCPU, old_callback.GetMPTR());
}

uint64 g_kpadLastRead[InputManager::kMaxWPADControllers] = {0};
bool g_kpadIsInited = true;

sint32 _KPADRead(uint32 channel, KPADStatus_t* samplingBufs, uint32 length, betype<KPAD_ERROR>* errResult)
{

	if (channel >= InputManager::kMaxWPADControllers)
	{
		debugBreakpoint();
		return 0;
	}
	
	if (g_kpadIsInited == false)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NOT_INITIALIZED;

		return 0;
	}

	const auto controller = InputManager::instance().get_wpad_controller(channel);
	if (!controller)
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_CONTROLLER;

		return 0;
	}

	// On console new input samples are only received every few ms and calling KPADRead(Ex) clears the internal queue regardless of length value
	// thus calling KPADRead(Ex) again too soon on the same channel will result in no data being returned
	// Games that depend on this: Affordable Space Adventures
	uint64 currentTime = coreinit::OSGetTime();
	uint64 timeDif = currentTime - g_kpadLastRead[channel];
	if(length == 0 || timeDif < coreinit::EspressoTime::ConvertNsToTimerTicks(1000000))
	{
		if (errResult)
			*errResult = KPAD_ERROR::NO_SAMPLE_DATA;
		return 0;
	}
	g_kpadLastRead[channel] = currentTime;

	memset(samplingBufs, 0x00, sizeof(KPADStatus_t));
	samplingBufs->wpadErr = WPAD_ERR_NONE;
	samplingBufs->data_format = controller->get_data_format();
	samplingBufs->devType = controller->get_device_type();
	if(!WindowSystem::InputConfigWindowHasFocus())
	{
		const auto btn_repeat = padscore::g_padscore.controller_data[channel].btn_repeat;
		controller->KPADRead(*samplingBufs, btn_repeat);
	}
	if (controller->get_device_type() == kWAPDevBalanceChecker)
		padscore::KPADReadBalanceBoard(*controller, *samplingBufs);

	if (errResult)
		*errResult = KPAD_ERROR::NONE;

	return 1;
}

void padscoreExport_KPADReadEx(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	ppcDefineParamPtr(errResult, betype<KPAD_ERROR>, 3);
	cemuLog_log(LogType::InputAPI, "KPADReadEx({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, errResult);
	osLib_returnFromFunction(hCPU, samplesRead);
}

void padscoreExport_KPADRead(PPCInterpreter_t* hCPU)
{
	ppcDefineParamU32(channel, 0);
	ppcDefineParamPtr(kpadStatus, KPADStatus_t, 1);
	ppcDefineParamU32(length, 2);
	cemuLog_log(LogType::InputAPI, "KPADRead({}, 0x{:x})", channel, length);

	sint32 samplesRead = _KPADRead(channel, kpadStatus, length, nullptr);
	osLib_returnFromFunction(hCPU, samplesRead);
}






#pragma endregion



namespace padscore
{
	void export_KPADEnableDPD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamS32(channel, 0);
		cemuLog_log(LogType::InputAPI, "KPADEnableDPD({})", channel);
		cemu_assert_debug(0 <= channel && channel < InputManager::kMaxWPADControllers);

		if(const auto controller = InputManager::instance().get_wpad_controller(channel))
		{
			if (controller->get_data_format() != kDataFormat_FREESTYLE && controller->get_data_format() != 0x1F)
				g_padscore.controller_data[channel].dpd_enabled = true;
		}
		

		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMplsWorkSize(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMplsWorkSize()");
		osLib_returnFromFunction(hCPU, 0x5FE0);
	}

	void KPADInitEx(KPADUnifiedWpadStatus_t ring_buffer[], uint32 length)
	{
		if (g_padscore.kpad_initialized)
			return;

		for (uint32 i = 0; i < InputManager::kMaxWPADControllers; i++)
		{
			g_padscore.controller_data[i].dpd_enabled = true;


		}

		g_kpad_ringbuffer = ring_buffer;
		g_kpad_ringbuffer_length = length;
		g_padscore.kpad_initialized = true;
	}

	void export_KPADInit(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADInit()");
		KPADInitEx(nullptr, 0);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADInitEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamMEMPTR(ring_buffer, KPADUnifiedWpadStatus_t, 0);
		ppcDefineParamU32(length, 1);
		cemuLog_log(LogType::InputAPI, "KPADInitEx(0x{:08x}, 0x{:x})", ring_buffer.GetMPTR(), length);
		KPADInitEx(ring_buffer.GetPtr(), length);
		osLib_returnFromFunction(hCPU, 0);
	}

	void WPADSetCallbackByKPAD(bool state)
	{
		g_wpad_callback_by_kpad = state;
	}

	void export_WPADSetCallbackByKPAD(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(state, 0);
		cemuLog_log(LogType::InputAPI, "WPADSetCallbackByKPAD({})", state);
		WPADSetCallbackByKPAD(state);
		osLib_returnFromFunction(hCPU, 0);
	}

	void export_KPADGetMaxControllers(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::InputAPI, "KPADGetMaxControllers()");
		sint32 max_controllers = g_padscore.max_controllers;
		osLib_returnFromFunction(hCPU, max_controllers);
	}

	bool WPADIsUsedCallbackByKPAD()
	{
		return g_wpad_callback_by_kpad == TRUE;
	}

	void* WPADSetSamplingCallback(sint32 channel, void* callback)
	{
		cemu_assert_debug(0 <= channel && channel < kKPADMaxControllers);

		const auto result = g_padscore.controller_data[channel].sampling_callback;
		g_padscore.controller_data[channel].sampling_callback = callback;
		return result.GetPtr();
	}

	void export_KPADSetMaxControllers(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(new_max_count, 0);
		cemuLog_log(LogType::InputAPI, "KPADSetMaxControllers({})", new_max_count);

		if (new_max_count != kKPADMaxControllers && new_max_count != kWPADMaxControllers)
		{
			debugBreakpoint();
			osLib_returnFromFunction(hCPU, -4);
			return;
		}


		const uint32 max_controllers = g_padscore.max_controllers;
		if (max_controllers == new_max_count)
		{
			osLib_returnFromFunction(hCPU, 0);
			return;
		}

		WPADSetCallbackByKPAD(FALSE);
		for (sint32 i = 0; i < kKPADMaxControllers; i++)
		{
			//WPADSetSamplingCallback(i, 0); TODO
		}

		g_padscore.max_controllers = new_max_count;
		
		WPADSetCallbackByKPAD(true);
		osLib_returnFromFunction(hCPU, new_max_count);
	}

	void export_WPADInit()
	{
		if (g_wpad_state != kWPADStateShutdown)
		{
			if (g_wpad_state == kWPADStateInitializing || g_wpad_state == kWPADStateAcquired)
				return;
		}
		g_wpad_state = kWPADStateInitializing;
	}


	enum class WPADStatus : sint32
	{
		Success = 0,
		NoController = -1,
		Busy = -2,
	};

	WPADStatus WPADIsMplsAttached(sint32 index, uint32be* attached, void* callback)
	{
		if (index >= kKPADMaxControllers)
			return WPADStatus::NoController;

		const auto controller = InputManager::instance().get_wpad_controller(index);
		*attached = controller && controller->is_mpls_attached();

		if (callback)
			PPCCoreCallback(MEMPTR(callback), index, controller ? WPADStatus::Success : WPADStatus::NoController);

		return WPADStatus::Success;
	}

	struct WPADAcc
	{
		sint32be x;
		sint32be y;
		sint32be z;
	};
	void WPADGetAccGravityUnit(sint32 index, uint32 type, WPADAcc* acc)
	{
		if (index >= kKPADMaxControllers)
			return;

		// default values for wiimote usually are around 99
		acc->x = 99;
		acc->y = 99;
		acc->z = 99;
	}



#pragma region Balance Board (BLC / WBC)

	// The Balance Board ("balance checker") is an emulated BalanceBoardController on WPAD
	// channel 3. The WPAD BLC calls serve its extension memory (calibration block). The WBC
	// library turns its raw load cell values into kilograms; this is a clean implementation of
	// that behaviour (see BalanceBoardMath.h), not a translation of Nintendo's code.

	enum WBCError : sint32
	{
		WBC_ERR_NONE = 0,
		WBC_ERR_NOT_CALIBRATED = -2,
		WBC_ERR_BAD_TEMPERATURE = -3,
		WBC_ERR_INVALID = -4,
	};

	struct
	{
		bool calibrated = false;
		BalanceBoardMath::Calibration cal{};
		double zero[BalanceBoardMath::kSensorCount]{}; // raw value of each load cell with nothing on the board
		bool zero_set = false;

		// logging
		uint32 read_count = 0;
		sint32 last_read_result = 0x7FFFFFFF;
		uint32 tgc_count = 0;
	} g_wbc;

	static std::shared_ptr<BalanceBoardController> GetBalanceBoard(uint32 channel)
	{
		return std::dynamic_pointer_cast<BalanceBoardController>(InputManager::instance().get_wpad_controller(channel));
	}

	// Gravity differs with latitude and the board measures force: the weight is scaled by a factor
	// for the console's region
	static double WBCGetGravityFactor()
	{
		switch (CafeSystem::GetPlatformRegion())
		{
		case CafeConsoleRegion::EUR:
			return 0.9977576;
		case CafeConsoleRegion::CHN:
			return 0.9994895;
		default:
			return 0.9990814;
		}
	}

	void WPADEnableWBC()
	{
	}

	void WPADRegisterBLCWorkarea(void* workarea)
	{
	}

	// TRUE if a board has been registered (paired) with the console
	uint32 WPADIsRegisteredBLC()
	{
		return TRUE;
	}

	// commands: 0x55 enable, 0xAA update, 0x00 start a temperature reading. The emulated board
	// always reports a valid temperature, so all of them simply succeed.
	sint32 WPADControlBLC(sint32 channel, uint32 command, MPTR callback)
	{
		const auto board = GetBalanceBoard(channel);
		const sint32 result = board ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
		cemuLog_log(LogType::InputAPI, "BalanceBoard: WPADControlBLC({}, 0x{:02x}) -> {}", channel, command, result);
		if (callback != MPTR_NULL)
			coreinitAsyncCallback_add(callback, 2, channel, (uint32)result);
		return result;
	}

	// reads `length` bytes of the board's calibration memory at `address` (0x20..0x3F: calibration
	// block and CRC, 0x60: reference temperature)
	sint32 WPADGetBLCalibration(sint32 channel, uint8* data, uint32 address, uint32 length, MPTR callback)
	{
		const auto board = GetBalanceBoard(channel);
		sint32 result = WPAD_ERR_NO_CONTROLLER;
		if (board && data)
		{
			const auto& mem = board->get_extension_memory();
			if (address < mem.size() && length <= mem.size() - address)
			{
				std::copy_n(mem.begin() + address, length, data);
				result = WPAD_ERR_NONE;
				cemuLog_log(LogType::InputAPI, "BalanceBoard: WPADGetBLCalibration({}, addr 0x{:02x}, len {}) -> {:02x}", channel, address, length, fmt::join(mem.begin() + address, mem.begin() + address + length, " "));
			}
			else
			{
				result = WPAD_ERR_INVALID;
				cemuLog_log(LogType::InputAPI, "BalanceBoard: WPADGetBLCalibration({}, addr 0x{:02x}, len {}): out of range", channel, address, length);
			}
		}
		else
			cemuLog_log(LogType::InputAPI, "BalanceBoard: WPADGetBLCalibration({}): no board on this channel", channel);

		if (callback != MPTR_NULL)
			coreinitAsyncCallback_add(callback, 2, channel, (uint32)result);
		return result;
	}

	// Reads and checks the board's calibration. TRUE if a board is there (the calibration is then
	// ready at once; WBCGetCalibrationStatus reports whether its CRC was valid).
	uint32 WBCSetupCalibration()
	{
		g_wbc.calibrated = false;
		const auto board = GetBalanceBoard(BalanceBoardController::kWPADChannel);
		if (!board)
		{
			cemuLog_log(LogType::InputAPI, "BalanceBoard: WBCSetupCalibration(): no Balance Board on WPAD channel {}", BalanceBoardController::kWPADChannel);
			return FALSE;
		}

		const auto& cal = g_wbc.cal = BalanceBoardMath::ParseCalibration(board->get_extension_memory());
		g_wbc.calibrated = cal.valid;
		g_wbc.read_count = 0;
		g_wbc.last_read_result = 0x7FFFFFFF;
		cemuLog_log(LogType::InputAPI, "BalanceBoard: WBCSetupCalibration(): CRC {:08x}/{:08x} {}, version {}, reference temperature {}",
			cal.crc_stored, cal.crc_computed, cal.valid ? "ok" : "MISMATCH", cal.version, cal.ref_temperature);
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
			cemuLog_log(LogType::InputAPI, "BalanceBoard:   sensor {}: {} / {} / {} at {} / {} / {} kg", i,
				cal.raw[i][0], cal.raw[i][1], cal.raw[i][2], cal.load_kg[0], cal.load_kg[1], cal.load_kg[2]);
		return TRUE;
	}

	uint32 WBCGetCalibrationStatus()
	{
		return g_wbc.calibrated ? TRUE : FALSE;
	}

	// sets the raw value of each load cell with nothing on the board
	sint32 WBCSetZEROPoint(float64be* zero, uint32 count)
	{
		if (!g_wbc.calibrated)
			return WBC_ERR_NOT_CALIBRATED;
		if (!zero || count < BalanceBoardMath::kSensorCount)
			return WBC_ERR_INVALID;

		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
			g_wbc.zero[i] = zero[i];
		g_wbc.zero_set = true;
		cemuLog_log(LogType::InputAPI, "BalanceBoard: WBCSetZEROPoint({:.2f}, {:.2f}, {:.2f}, {:.2f})", g_wbc.zero[0], g_wbc.zero[1], g_wbc.zero[2], g_wbc.zero[3]);
		return WBC_ERR_NONE;
	}

	static sint32 WBCConvert(const WPADBLStatus_t& status, double weight[BalanceBoardMath::kSensorCount], sint32& total_raw)
	{
		uint16 raw[BalanceBoardMath::kSensorCount];
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
		{
			raw[i] = status.press[i];
			weight[i] = BalanceBoardMath::RawToKg(g_wbc.cal, i, raw[i], g_wbc.zero[i]);
		}
		total_raw = BalanceBoardMath::RawLoadAboveZeroKg(g_wbc.cal, raw);
		return total_raw > BalanceBoardMath::kOnBoardRawThreshold ? 1 : 0;
	}

	// raw load cell values -> kilograms per load cell. 1: someone is on the board, 0: nobody.
	sint32 WBCRead(WPADBLStatus_t* status, float64be* weight, uint32 count)
	{
		if (!g_wbc.calibrated)
			return WBC_ERR_NOT_CALIBRATED;
		if (!status || !weight || count < BalanceBoardMath::kSensorCount)
			return WBC_ERR_INVALID;

		double w[BalanceBoardMath::kSensorCount];
		sint32 total_raw;
		const sint32 result = WBCConvert(*status, w, total_raw);
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
			weight[i] = w[i];

		g_wbc.read_count++;
		if (g_wbc.read_count <= 3 || result != g_wbc.last_read_result || (g_wbc.read_count % 600) == 0)
		{
			cemuLog_log(LogType::InputAPI, "BalanceBoard: WBCRead #{}: raw {} {} {} {} -> {:.2f} {:.2f} {:.2f} {:.2f} kg (sum {:.2f}), load {} -> {}{}",
				g_wbc.read_count, (uint16)status->press[0], (uint16)status->press[1], (uint16)status->press[2], (uint16)status->press[3],
				w[0], w[1], w[2], w[3], w[0] + w[1] + w[2] + w[3], total_raw, result, g_wbc.zero_set ? "" : " (no zero point yet)");
		}
		g_wbc.last_read_result = result;
		return result;
	}

	// temperature and gravity corrected (TGC) total weight
	sint32 WBCGetTGCWeight(float64be* tgcWeight, WPADBLStatus_t* status, double weight)
	{
		if (!g_wbc.calibrated)
			return WBC_ERR_NOT_CALIBRATED;
		if (!tgcWeight || !status)
			return WBC_ERR_INVALID;
		if (BalanceBoardMath::IsInvalidTemperature(status->temp))
			return WBC_ERR_BAD_TEMPERATURE;

		const double result = BalanceBoardMath::TemperatureCorrected(weight, status->temp, g_wbc.cal.ref_temperature) * WBCGetGravityFactor();
		*tgcWeight = result;
		g_wbc.tgc_count++;
		if (g_wbc.tgc_count <= 10 || (g_wbc.tgc_count % 300) == 0)
			cemuLog_log(LogType::InputAPI, "BalanceBoard: WBCGetTGCWeight({:.3f} kg, temperature {}) -> {:.3f} kg", weight, status->temp, result);
		return WBC_ERR_NONE;
	}

	// WPADBLStatus::battery -> 0 (empty) to 4 (full)
	sint32 WBCGetBatteryLevel(uint8 battery)
	{
		return BalanceBoardMath::BatteryRawToLevel(battery);
	}

	// KPAD on a board: weights in ex_status.bl, like the KPAD library does it with WBC
	void KPADReadBalanceBoard(WPADController& controller, KPADStatus_t& status)
	{
		if (controller.get_data_format() != kDataFormat_BALANCE_CHECKER)
			return;
		WPADBLStatus_t bl{};
		controller.WPADRead(&bl);
		auto& ex = status.ex_status.bl;
		if (!g_wbc.calibrated)
		{
			ex.weight_err = WBC_ERR_NOT_CALIBRATED;
			ex.tgc_weight_err = WBC_ERR_NOT_CALIBRATED;
			return;
		}

		double w[BalanceBoardMath::kSensorCount];
		sint32 total_raw;
		ex.weight_err = WBCConvert(bl, w, total_raw);
		double sum = 0.0;
		for (int i = 0; i < BalanceBoardMath::kSensorCount; i++)
		{
			ex.weight[i] = w[i];
			ex.weight_ave[i] = w[i];
			sum += w[i];
		}
		if (g_wbc.zero_set && !BalanceBoardMath::IsInvalidTemperature(bl.temp))
		{
			ex.tgc_weight = BalanceBoardMath::TemperatureCorrected(sum, bl.temp, g_wbc.cal.ref_temperature) * WBCGetGravityFactor();
			ex.tgc_weight_err = 0;
		}
		else
			ex.tgc_weight_err = WBC_ERR_NOT_CALIBRATED;
	}

#pragma endregion

	void TickFunction(PPCInterpreter_t* hCPU)
	{
		auto& instance = InputManager::instance();
		// test for connected/disconnected controllers
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].connectCallback) 
			{
				if(!g_padscore.controller_data[i].disconnectCalled)
				{
					g_padscore.controller_data[i].disconnectCalled = true;
					cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
					PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
					continue;
				}

				if (const auto controller = instance.get_wpad_controller(i))
				{
					if (controller->m_status == WPADController::ConnectCallbackStatus::ReportDisconnect || controller->was_home_button_down()) // fixed!
					{
						controller->m_status = WPADController::ConnectCallbackStatus::ReportConnect;

						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NO_CONTROLLER);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NO_CONTROLLER);
						
					}
					else if (controller->m_status == WPADController::ConnectCallbackStatus::ReportConnect) 
					{
						controller->m_status = WPADController::ConnectCallbackStatus::None;
						cemuLog_log(LogType::InputAPI, "Calling WPADConnectCallback({}, {})", i, WPAD_ERR_NONE);
						PPCCoreCallback(g_padscore.controller_data[i].connectCallback, i, WPAD_ERR_NONE);
					}
				}
			}
		}

		// test for connected/disconnected extensions
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].extension_callback) 
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					if (controller->m_extension_status == WPADController::ConnectCallbackStatus::ReportConnect)
					{
						controller->m_extension_status = WPADController::ConnectCallbackStatus::None;
						cemuLog_log(LogType::InputAPI, "Calling WPADextensionCallback({})", i);
						PPCCoreCallback(g_padscore.controller_data[i].extension_callback, i, controller->get_device_type());
					}
				}
			}
		}

		// call sampling callback
		for (auto i = 0; i < InputManager::kMaxWPADControllers; ++i)
		{
			if (g_padscore.controller_data[i].sampling_callback)
			{
				if (const auto controller = instance.get_wpad_controller(i))
				{
					cemuLog_log(LogType::InputAPI, "Calling WPADsamplingCallback({})", i);
					PPCCoreCallback(g_padscore.controller_data[i].sampling_callback, i);
				}
			}
		}

		osLib_returnFromFunction(hCPU, 0);
	}
	void start()
	{
		OSCreateAlarm(&g_padscore.alarm);
		const uint64 start_tick = coreinit::OSGetTime();
		const uint64 period_tick = coreinit::EspressoTime::GetTimerClock() / 200; // every 5ms
		MPTR handler = PPCInterpreter_makeCallableExportDepr(TickFunction);
		OSSetPeriodicAlarm(&g_padscore.alarm, start_tick, period_tick, handler);
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "padscore";
		}

		void RPLMapped() override
		{
			cafeExportRegister("padscore", WPADIsMplsAttached, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetAccGravityUnit, LogType::InputAPI);

			// Balance Board (BLC = balance checker) and its WBC library
			cafeExportRegister("padscore", WPADEnableWBC, LogType::InputAPI);
			cafeExportRegister("padscore", WPADRegisterBLCWorkarea, LogType::InputAPI);
			cafeExportRegister("padscore", WPADIsRegisteredBLC, LogType::InputAPI);
			cafeExportRegister("padscore", WPADControlBLC, LogType::InputAPI);
			cafeExportRegister("padscore", WPADGetBLCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", WBCSetupCalibration, LogType::InputAPI);
			cafeExportRegister("padscore", WBCGetCalibrationStatus, LogType::InputAPI);
			cafeExportRegister("padscore", WBCSetZEROPoint, LogType::InputAPI);
			cafeExportRegister("padscore", WBCRead, LogType::Placeholder); // every frame: logs its own summary
			cafeExportRegister("padscore", WBCGetTGCWeight, LogType::InputAPI);
			cafeExportRegister("padscore", WBCGetBatteryLevel, LogType::InputAPI);

			// wpad
			//osLib_addFunction("padscore", "WPADInit", padscore::export_WPADInit);

			// kpad
			osLib_addFunction("padscore", "KPADSetMaxControllers", padscore::export_KPADSetMaxControllers);
			osLib_addFunction("padscore", "KPADGetMaxControllers", padscore::export_KPADGetMaxControllers);
			osLib_addFunction("padscore", "KPADEnableDPD", padscore::export_KPADEnableDPD);
			osLib_addFunction("padscore", "KPADGetMplsWorkSize", padscore::export_KPADGetMplsWorkSize);
			osLib_addFunction("padscore", "KPADInit", padscore::export_KPADInit);
			osLib_addFunction("padscore", "KPADInitEx", padscore::export_KPADInitEx);

			osLib_addFunction("padscore", "KPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "KPADReadEx", padscoreExport_KPADReadEx);
			osLib_addFunction("padscore", "KPADRead", padscoreExport_KPADRead);
			osLib_addFunction("padscore", "KPADGetUnifiedWpadStatus", padscoreExport_KPADGetUnifiedWpadStatus);
			osLib_addFunction("padscore", "KPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "KPADSetBtnRepeat", padscoreExport_KPADSetBtnRepeat);

			osLib_addFunction("padscore", "WPADGetBatteryLevel", padscoreExport_WPADGetBatteryLevel);
			osLib_addFunction("padscore", "WPADControlMotor", padscoreExport_WPADControlMotor);
			osLib_addFunction("padscore", "WPADIsMotorEnabled", padscoreExport_WPADIsMotorEnabled);
			osLib_addFunction("padscore", "WPADGetStatus", padscoreExport_WPADGetStatus);
			osLib_addFunction("padscore", "WPADProbe", padscoreExport_WPADProbe);
			osLib_addFunction("padscore", "WPADGetInfoAsync", padscoreExport_WPADGetInfoAsync);
			osLib_addFunction("padscore", "WPADGetInfo", padscoreExport_WPADGetInfo);
			osLib_addFunction("padscore", "WPADSetConnectCallback", padscoreExport_KPADSetConnectCallback);
			osLib_addFunction("padscore", "WPADSetDataFormat", padscoreExport_WPADSetDataFormat);
			osLib_addFunction("padscore", "WPADGetDataFormat", padscoreExport_WPADGetDataFormat);
			osLib_addFunction("padscore", "WPADRead", padscoreExport_WPADRead);
			osLib_addFunction("padscore", "WPADSetExtensionCallback", padscoreExport_WPADSetExtensionCallback);
			osLib_addFunction("padscore", "WPADSetSamplingCallback", padscoreExport_KPADSetSamplingCallback);
			osLib_addFunction("padscore", "WPADControlDpd", padscoreExport_WPADControlDpd);

			osLib_addFunction("padscore", "WPADSetCallbackByKPAD", padscore::export_WPADSetCallbackByKPAD);
		};

	}s_COSCoreinitModule;

	COSModule* GetModule()
	{
		return &s_COSCoreinitModule;
	}

}
