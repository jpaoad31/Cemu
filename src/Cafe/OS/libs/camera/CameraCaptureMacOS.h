#pragma once

// AVFoundation capture (CameraCaptureMacOS.mm). Plain types only, so the
// Objective-C++ side doesn't need Cemu's precompiled header.

#include <cstdint>
#include <string>

namespace camera::macos
{
	struct Capture;

	enum class StartResult
	{
		Started,           // session running (or starting)
		PermissionPending, // the system is asking the user; starts once granted
		PermissionDenied,
		NoDevice,
		Failed,
	};

	Capture* CaptureCreate();
	void CaptureDestroy(Capture* capture);

	StartResult CaptureStart(Capture* capture);
	void CaptureStop(Capture* capture);

	// latest frame as NV12, center-cropped to the target aspect ratio and
	// scaled to width x height (both even); false if no frame yet
	bool CaptureGetNV12(Capture* capture, uint8_t* dst, uint32_t width, uint32_t height, uint32_t pitch);

	std::string CaptureDeviceName(Capture* capture);
	uint64_t CaptureFrameCount(Capture* capture);
	// the size of the host frames ("1280x720"), empty before the first frame
	std::string CaptureFrameFormat(Capture* capture);
}
