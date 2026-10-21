#include "Common/precompiled.h"
#include "Cafe/OS/common/OSCommon.h"
#include "camera.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "CameraBackend.h"

namespace camera
{

	// layouts as in WUT's camera.h (CAMSetupInfo, CAMSurface, CAMEventData);
	// Wii Fit U fills height 480 at +4 and width 640 at +8
	struct CAMInitInfo_t
	{
		/* +0x00 */ uint32be streamType;
		/* +0x04 */ uint32be height;
		/* +0x08 */ uint32be width;

		/* +0x0C */ uint32be workMemorySize;
		/* +0x10 */ MEMPTR<void> workMemory;

		/* +0x14 */ uint32be handlerFuncPtr;

		/* +0x18 */ uint32be forceDrc;
		/* +0x1C */ uint32be fps; // CAM_FPS_15 = 0, CAM_FPS_30 = 1

		/* +0x20 */ uint32be threadAffinity;
		/* +0x24 */ uint8 padding24[0x10];
	};
	static_assert(sizeof(CAMInitInfo_t) == 0x34);

	#define CAM_FPS_15 0
	#define CAM_FPS_30 1

	// the GamePad camera's image: NV12, 640x480, rows padded to 768 bytes
	// (Wii Fit U allocates 768*480*3/2 per surface and reads UV at +768*480)
	constexpr uint32 CAM_WIDTH = 640;
	constexpr uint32 CAM_HEIGHT = 480;
	constexpr uint32 CAM_PITCH = 768;

	struct CAMTargetSurface 
	{
		/* +0x00 */ uint32be surfaceSize;
		/* +0x04 */ MEMPTR<void> surfacePtr;
		/* +0x08 */ uint32be height; // games may leave these 0
		/* +0x0C */ uint32be width;
		/* +0x10 */ uint32be pitch;
		/* +0x14 */ uint32be alignment;
		/* +0x18 */ uint32be tileMode;
		/* +0x1C */ uint32be pixelFormat;
	};

	struct CAMCallbackParam 
	{
		// type 0 - decode done | field1 - surface buffer, field2 - camera handle, field3 - decode failed (BOOL)
		// type 1 - GamePad detached | field1 - connected (FALSE), field2 - camera handle


		/* +0x0 */ uint32be type; // 0 -> Frame decoded
		/* +0x4 */ uint32be field1;
		/* +0x8 */ uint32be field2;
		/* +0xC */ uint32be field3;
	};


	#define CAM_ERROR_SUCCESS			0
	#define CAM_ERROR_INVALID_HANDLE		-8

	std::vector<struct CameraInstance*> g_table_cameraHandles;
	std::vector<struct CameraInstance*> g_activeCameraInstances;
	std::recursive_mutex g_mutex_camera;
	std::atomic_int g_cameraCounter{ 0 };
	SysAllocator<coreinit::OSAlarm_t, 1> g_alarm_camera;
	SysAllocator<CAMCallbackParam, 1> g_cameraHandlerParam;
	// host camera, created on the first CAMOpen and running while any instance is open
	std::unique_ptr<CameraBackend> g_cameraBackend;
	bool g_cameraBackendCreated{false};
	bool g_cameraBackendRunning{false};

	// start the host camera while any instance is open, stop it otherwise
	void UpdateBackendState()
	{
		std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
		const bool wantRunning = !g_activeCameraInstances.empty();
		if (wantRunning && !g_cameraBackendCreated)
		{
			g_cameraBackend = CreateCameraBackend();
			g_cameraBackendCreated = true;
			if (!g_cameraBackend)
				cemuLog_log(LogType::Force, "Camera: no host camera support on this platform; the GamePad camera shows no image");
		}
		if (!g_cameraBackend || wantRunning == g_cameraBackendRunning)
			return;
		if (wantRunning)
			g_cameraBackendRunning = g_cameraBackend->Start();
		else
		{
			g_cameraBackend->Stop();
			g_cameraBackendRunning = false;
		}
	}

	CameraInstance* GetCameraInstanceByHandle(sint32 camHandle)
	{
		std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
		if (camHandle <= 0)
			return nullptr;
		camHandle -= 1;
		if (camHandle >= g_table_cameraHandles.size())
			return nullptr;
		return g_table_cameraHandles[camHandle];
	}

	struct CameraInstance 
	{
		CameraInstance(uint32 frameWidth, uint32 frameHeight, uint32 fps, MPTR handlerFunc) : width(frameWidth), height(frameHeight), fps(fps), handlerFunc(handlerFunc) { AcquireHandle(); };
		~CameraInstance() { if (isOpen) { CloseCam(); } ReleaseHandle(); };

		sint32 handle{ 0 };
		uint32 width;
		uint32 height;
		uint32 fps;
		uint32 tickCounter{0}; // 60 Hz ticks since the last delivered frame
		bool isOpen{false};
		std::queue<CAMTargetSurface> queue_targetSurfaces;
		MPTR handlerFunc;

		bool OpenCam()
		{
			if (isOpen)
				return false;
			isOpen = true;
			g_activeCameraInstances.push_back(this);
			UpdateBackendState();
			return true;
		}

		bool CloseCam()
		{
			if (!isOpen)
				return false;
			isOpen = false;
			vectorRemoveByValue(g_activeCameraInstances, this);
			UpdateBackendState();
			return true;
		}

		void QueueTargetSurface(CAMTargetSurface* targetSurface)
		{
			std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
			cemu_assert_debug(queue_targetSurfaces.size() < 100); // check for sane queue length
			queue_targetSurfaces.push(*targetSurface);
		}

	private:
		void AcquireHandle()
		{
			std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
			for (uint32 i = 0; i < g_table_cameraHandles.size(); i++)
			{
				if (g_table_cameraHandles[i] == nullptr)
				{
					g_table_cameraHandles[i] = this;
					this->handle = i + 1;
					return;
				}
			}
			this->handle = (sint32)(g_table_cameraHandles.size() + 1);
			g_table_cameraHandles.push_back(this);
		}

		void ReleaseHandle()
		{
			for (uint32 i = 0; i < g_table_cameraHandles.size(); i++)
			{
				if (g_table_cameraHandles[i] == this)
				{
					g_table_cameraHandles[i] = nullptr;
					return;
				}
			}
			cemu_assert_debug(false);
		}
	};

	// write the latest host frame into a surface the game submitted; black if
	// there is none (no backend, no permission, first frame not there yet)
	bool FillSurface(const CAMTargetSurface& surface)
	{
		const uint32 size = surface.surfaceSize;
		uint8* dst = (uint8*)surface.surfacePtr.GetPtr();
		if (!dst || size == 0)
			return false;
		const uint32 width = surface.width != 0 ? (uint32)surface.width : CAM_WIDTH;
		const uint32 height = surface.height != 0 ? (uint32)surface.height : CAM_HEIGHT;
		uint32 pitch = surface.pitch;
		if (pitch == 0)
			pitch = ((uint64)CAM_PITCH * height * 3 / 2 <= size) ? CAM_PITCH : width;
		if (width < 2 || height < 2 || pitch < width || (uint64)pitch * height * 3 / 2 > size)
			return false;
		if (g_cameraBackendRunning && g_cameraBackend && g_cameraBackend->GetLatestFrameNV12(dst, width & ~1u, height & ~1u, pitch))
			return true;
		// black: Y 16, UV 128 (video range)
		memset(dst, 16, (size_t)pitch * height);
		memset(dst + (size_t)pitch * height, 128, (size_t)pitch * height / 2);
		return true;
	}

	sint32 CAMGetMemReq(void* ukn)
	{
		return 1 * 1024; // always return 1KB
	}

	sint32 CAMCheckMemSegmentation(void* base, uint32 size)
	{
		return CAM_ERROR_SUCCESS; // always return success
	}

	void ppcCAMUpdate60(PPCInterpreter_t* hCPU)
	{
		// update all open camera instances
		size_t numCamInstances = g_activeCameraInstances.size();
		//for (auto& itr : g_activeCameraInstances)
		for(size_t i=0; i<numCamInstances; i++)
		{
			std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
			if (i >= g_activeCameraInstances.size())
				break;
			CameraInstance* camInstance = g_activeCameraInstances[i];
			// deliver at the requested rate: every 2nd tick for 30 fps, every 4th for 15 fps
			const uint32 ticksPerFrame = camInstance->fps == CAM_FPS_15 ? 4 : 2;
			if (++camInstance->tickCounter < ticksPerFrame)
				continue;
			if (camInstance->queue_targetSurfaces.empty())
				continue;
			camInstance->tickCounter = 0;
			auto& targetSurface = camInstance->queue_targetSurfaces.front();
			const bool filled = FillSurface(targetSurface);
			g_cameraHandlerParam->type = 0;
			g_cameraHandlerParam->field1 = targetSurface.surfacePtr.GetMPTR();
			g_cameraHandlerParam->field2 = (uint32)camInstance->handle;
			g_cameraHandlerParam->field3 = filled ? 0 : 1;
			cemu_assert_debug(camInstance->handlerFunc != MPTR_NULL);
			camInstance->queue_targetSurfaces.pop();
			_lock.unlock();
			PPCCoreCallback(camInstance->handlerFunc, g_cameraHandlerParam.GetPtr());
		}
		osLib_returnFromFunction(hCPU, 0);
	}


	sint32 CAMInit(uint32 cameraId, CAMInitInfo_t* camInitInfo, uint32be* error)
	{
		CameraInstance* camInstance = new CameraInstance(camInitInfo->width, camInitInfo->height, camInitInfo->fps, camInitInfo->handlerFuncPtr);
		*error = 0; // Hunter's Trophy 2 will fail to boot if we don't set this
		std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
		if (g_cameraCounter == 0)
		{
			coreinit::OSCreateAlarm(g_alarm_camera.GetPtr());
			coreinit::OSSetPeriodicAlarm(g_alarm_camera.GetPtr(), coreinit::OSGetTime(), (uint64)ESPRESSO_TIMER_CLOCK / 60ull, RPLLoader_MakePPCCallable(ppcCAMUpdate60));
		}
		g_cameraCounter++;

		return camInstance->handle;
	}

	sint32 CAMExit(sint32 camHandle)
	{
		CameraInstance* camInstance = GetCameraInstanceByHandle(camHandle);
		if (!camInstance)
			return CAM_ERROR_INVALID_HANDLE;
		CAMClose(camHandle);
		delete camInstance;

		std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
		g_cameraCounter--;
		if (g_cameraCounter == 0)
			coreinit::OSCancelAlarm(g_alarm_camera.GetPtr());
		return CAM_ERROR_SUCCESS;
	}

	sint32 CAMOpen(sint32 camHandle)
	{
		CameraInstance* camInstance = GetCameraInstanceByHandle(camHandle);
		if (!camInstance)
			return CAM_ERROR_INVALID_HANDLE;
		camInstance->OpenCam();
		return CAM_ERROR_SUCCESS;
	}

	sint32 CAMClose(sint32 camHandle)
	{
		CameraInstance* camInstance = GetCameraInstanceByHandle(camHandle);
		if (!camInstance)
			return CAM_ERROR_INVALID_HANDLE;
		camInstance->CloseCam();
		return CAM_ERROR_SUCCESS;
	}

	sint32 CAMSubmitTargetSurface(sint32 camHandle, CAMTargetSurface* targetSurface)
	{
		CameraInstance* camInstance = GetCameraInstanceByHandle(camHandle);
		if (!camInstance)
			return CAM_ERROR_INVALID_HANDLE;
		
		camInstance->QueueTargetSurface(targetSurface);

		return CAM_ERROR_SUCCESS;
	}

	void reset()
	{
		g_cameraCounter = 0;
		std::unique_lock<std::recursive_mutex> _lock(g_mutex_camera);
		if (g_cameraBackend && g_cameraBackendRunning)
			g_cameraBackend->Stop();
		g_cameraBackend.reset();
		g_cameraBackendCreated = false;
		g_cameraBackendRunning = false;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "camera";
		}

		void RPLMapped() override
		{
			cafeExportRegister("camera", CAMGetMemReq, LogType::Placeholder);
			cafeExportRegister("camera", CAMCheckMemSegmentation, LogType::Placeholder);
			cafeExportRegister("camera", CAMInit, LogType::Placeholder);
			cafeExportRegister("camera", CAMExit, LogType::Placeholder);
			cafeExportRegister("camera", CAMOpen, LogType::Placeholder);
			cafeExportRegister("camera", CAMClose, LogType::Placeholder);
			cafeExportRegister("camera", CAMSubmitTargetSurface, LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				reset();
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				// todo
			}
		}
	}s_COScameraModule;

	COSModule* GetModule()
	{
		return &s_COScameraModule;
	}
}

