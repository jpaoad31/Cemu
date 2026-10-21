#include "CameraCaptureMacOS.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

// Frames are requested as NV12 (420YpCbCr8BiPlanar, video range), the Wii U
// camera's format, so conversion is only crop + scale of the two planes.

namespace camera::macos
{
	struct Capture;
	static void StoreFrame(Capture* capture, CVPixelBufferRef pixelBuffer);
}

@interface CemuCameraDelegate : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate>
{
@public
	camera::macos::Capture* capture;
}
@end

@implementation CemuCameraDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection
{
	CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (pixelBuffer)
		camera::macos::StoreFrame(capture, pixelBuffer);
}
@end

namespace camera::macos
{
	struct Capture
	{
		AVCaptureSession* session{nil};
		CemuCameraDelegate* delegate{nil};
		dispatch_queue_t queue{nullptr};
		std::string deviceName;

		std::mutex frameMutex;
		// the latest host frame, tightly packed NV12 at its own size
		std::vector<uint8_t> frameY, frameUV;
		uint32_t frameWidth{0}, frameHeight{0};
		uint64_t frameCount{0};

		// all session work runs on the serial queue; this only says whether the
		// session should be running
		std::atomic<bool> wantRunning{false};
	};

	static void StoreFrame(Capture* capture, CVPixelBufferRef pixelBuffer)
	{
		if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
			CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
			return;
		if (CVPixelBufferGetPlaneCount(pixelBuffer) < 2)
			return;
		CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
		const uint32_t w = (uint32_t)CVPixelBufferGetWidthOfPlane(pixelBuffer, 0) & ~1u;
		const uint32_t h = (uint32_t)CVPixelBufferGetHeightOfPlane(pixelBuffer, 0) & ~1u;
		const uint8_t* srcY = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
		const uint8_t* srcUV = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
		const size_t pitchY = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
		const size_t pitchUV = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
		if (srcY && srcUV && w && h)
		{
			std::lock_guard<std::mutex> lock(capture->frameMutex);
			capture->frameY.resize((size_t)w * h);
			capture->frameUV.resize((size_t)w * h / 2);
			for (uint32_t y = 0; y < h; y++)
				memcpy(capture->frameY.data() + (size_t)y * w, srcY + y * pitchY, w);
			for (uint32_t y = 0; y < h / 2; y++)
				memcpy(capture->frameUV.data() + (size_t)y * w, srcUV + y * pitchUV, w);
			capture->frameWidth = w;
			capture->frameHeight = h;
			capture->frameCount++;
		}
		CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
	}

	static bool SetupSession(Capture* capture)
	{
		AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
		if (!device)
			return false;
		capture->deviceName = device.localizedName ? std::string(device.localizedName.UTF8String) : "camera";

		NSError* error = nil;
		AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
		if (!input)
			return false;

		AVCaptureSession* session = [[AVCaptureSession alloc] init];
		[session beginConfiguration];
		// 640x480 is the Wii U camera's size; fall back to whatever the device offers
		if ([session canSetSessionPreset:AVCaptureSessionPreset640x480])
			session.sessionPreset = AVCaptureSessionPreset640x480;
		else if ([session canSetSessionPreset:AVCaptureSessionPresetMedium])
			session.sessionPreset = AVCaptureSessionPresetMedium;
		if (![session canAddInput:input])
		{
			[session commitConfiguration];
			[session release];
			return false;
		}
		[session addInput:input];

		AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
		output.videoSettings = @{ (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
		output.alwaysDiscardsLateVideoFrames = YES;
		capture->delegate = [[CemuCameraDelegate alloc] init];
		capture->delegate->capture = capture;
		[output setSampleBufferDelegate:capture->delegate queue:capture->queue];
		if (![session canAddOutput:output])
		{
			[output release];
			[session commitConfiguration];
			[session release];
			return false;
		}
		[session addOutput:output];
		[output release];
		[session commitConfiguration];
		capture->session = session;
		return true;
	}

	static void RunSession(Capture* capture)
	{
		dispatch_async(capture->queue, ^{
			if (!capture->wantRunning)
				return;
			if (!capture->session && !SetupSession(capture))
				return;
			if (!capture->session.running)
				[capture->session startRunning];
		});
	}

	Capture* CaptureCreate()
	{
		Capture* capture = new Capture();
		capture->queue = dispatch_queue_create("info.cemu.camera", DISPATCH_QUEUE_SERIAL);
		return capture;
	}

	void CaptureDestroy(Capture* capture)
	{
		if (!capture)
			return;
		CaptureStop(capture);
		// wait for queued session work and frame callbacks to finish
		dispatch_sync(capture->queue, ^{
			if (capture->session)
			{
				[capture->session release];
				capture->session = nil;
			}
		});
		if (capture->delegate)
			[capture->delegate release];
		dispatch_release(capture->queue);
		delete capture;
	}

	StartResult CaptureStart(Capture* capture)
	{
		if (![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo])
			return StartResult::NoDevice;
		capture->wantRunning = true;
		switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo])
		{
		case AVAuthorizationStatusAuthorized:
			RunSession(capture);
			return StartResult::Started;
		case AVAuthorizationStatusNotDetermined:
			[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
				if (granted)
					RunSession(capture);
			}];
			return StartResult::PermissionPending;
		default: // denied or restricted
			return StartResult::PermissionDenied;
		}
	}

	void CaptureStop(Capture* capture)
	{
		capture->wantRunning = false;
		dispatch_async(capture->queue, ^{
			if (capture->session && capture->session.running && !capture->wantRunning)
				[capture->session stopRunning];
		});
	}

	bool CaptureGetNV12(Capture* capture, uint8_t* dst, uint32_t width, uint32_t height, uint32_t pitch)
	{
		std::lock_guard<std::mutex> lock(capture->frameMutex);
		const uint32_t sw = capture->frameWidth, sh = capture->frameHeight;
		if (!sw || !sh || width < 2 || height < 2 || pitch < width)
			return false;
		// center crop of the source to the target's aspect ratio
		uint32_t cw = sw, ch = sh;
		if ((uint64_t)sw * height > (uint64_t)sh * width)
			cw = (uint32_t)((uint64_t)sh * width / height) & ~1u;
		else
			ch = (uint32_t)((uint64_t)sw * height / width) & ~1u;
		const uint32_t cx = ((sw - cw) / 2) & ~1u, cy = ((sh - ch) / 2) & ~1u;

		// Y: nearest neighbour
		std::vector<uint32_t> mapX(width);
		for (uint32_t x = 0; x < width; x++)
			mapX[x] = cx + (uint32_t)((uint64_t)x * cw / width);
		for (uint32_t y = 0; y < height; y++)
		{
			const uint8_t* srcRow = capture->frameY.data() + (size_t)(cy + (uint64_t)y * ch / height) * sw;
			uint8_t* dstRow = dst + (size_t)y * pitch;
			for (uint32_t x = 0; x < width; x++)
				dstRow[x] = srcRow[mapX[x]];
		}
		// UV: half resolution, interleaved pairs
		uint8_t* dstUV = dst + (size_t)pitch * height;
		for (uint32_t y = 0; y < height / 2; y++)
		{
			const uint32_t sy = (cy / 2) + (uint32_t)((uint64_t)y * (ch / 2) / (height / 2));
			const uint8_t* srcRow = capture->frameUV.data() + (size_t)sy * sw;
			uint8_t* dstRow = dstUV + (size_t)y * pitch;
			for (uint32_t x = 0; x < width / 2; x++)
			{
				const uint32_t sx = (cx / 2) + (uint32_t)((uint64_t)x * (cw / 2) / (width / 2));
				dstRow[x * 2 + 0] = srcRow[sx * 2 + 0];
				dstRow[x * 2 + 1] = srcRow[sx * 2 + 1];
			}
		}
		return true;
	}

	std::string CaptureDeviceName(Capture* capture)
	{
		if (!capture->deviceName.empty())
			return capture->deviceName;
		AVCaptureDevice* device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
		return (device && device.localizedName) ? std::string(device.localizedName.UTF8String) : std::string();
	}

	uint64_t CaptureFrameCount(Capture* capture)
	{
		std::lock_guard<std::mutex> lock(capture->frameMutex);
		return capture->frameCount;
	}

	std::string CaptureFrameFormat(Capture* capture)
	{
		std::lock_guard<std::mutex> lock(capture->frameMutex);
		if (!capture->frameWidth)
			return {};
		return std::to_string(capture->frameWidth) + "x" + std::to_string(capture->frameHeight);
	}
}
