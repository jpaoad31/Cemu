#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

void LatteThread_Exit();

SlimRWLock swl_gpuAsyncCommands;

typedef struct  
{
	uint32 type;
	union
	{
		struct
		{
			MPTR physAddr;
			MPTR mipAddr;
			uint32 swizzle;
			sint32 format;
			sint32 width;
			sint32 height;
			sint32 depth;
			uint32 pitch;
			uint32 slice;
			sint32 dim;
			Latte::E_HWTILEMODE tilemode;
			sint32 aa;
			sint32 level;
		}forceTextureReadback;

		struct
		{
			uint64 shaderBaseHash; 
			uint64 shaderAuxHash; 
			LatteConst::ShaderType shaderType;
		}deleteShader;

		struct
		{
			LatteSurfaceCopyParam src;
			LatteSurfaceCopyParam dst;
			LatteSurfaceCopyRect rect;
		}textureCopy;

		struct
		{
			MPTR physAddr;
		}cpuReadback;
	};
}LatteAsyncCommand_t;

#define ASYNC_CMD_FORCE_TEXTURE_READBACK		1
#define ASYNC_CMD_DELETE_SHADER					2
#define ASYNC_CMD_TEXTURE_COPY					3
#define ASYNC_CMD_CPU_READBACK					4

std::queue<LatteAsyncCommand_t> LatteAsyncCommandQueue;

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_FORCE_TEXTURE_READBACK;
	
	asyncCommand.forceTextureReadback.physAddr = physAddr;
	asyncCommand.forceTextureReadback.mipAddr = mipAddr;
	asyncCommand.forceTextureReadback.swizzle = swizzle;
	asyncCommand.forceTextureReadback.format = format;
	asyncCommand.forceTextureReadback.width = width;
	asyncCommand.forceTextureReadback.height = height;
	asyncCommand.forceTextureReadback.depth = depth;
	asyncCommand.forceTextureReadback.pitch = pitch;
	asyncCommand.forceTextureReadback.slice = slice;
	asyncCommand.forceTextureReadback.dim = dim;
	asyncCommand.forceTextureReadback.tilemode = tilemode;
	asyncCommand.forceTextureReadback.aa = aa;
	asyncCommand.forceTextureReadback.level = level;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

/*
 * The CPU is about to read GPU-rendered texture data at physAddr (GX2Invalidate with the CPU and
 * color-buffer flags). Cemu keeps render targets on the host GPU and mirrors only linear ones back to
 * RAM, so read back any GPU-written tiled color texture that starts there. Wii Fit U renders Mii icons
 * into a tiled RGBA8 target and then thresholds their alpha on the CPU (RPSysMiiIcon::BinarizationAlpha):
 * without this the CPU edited stale RAM and the next reload replaced the icon with that garbage.
 */
void LatteAsyncCommands_queueCpuReadback(MPTR physAddr)
{
	LatteAsyncCommand_t asyncCommand = {};
	asyncCommand.type = ASYNC_CMD_CPU_READBACK;
	asyncCommand.cpuReadback.physAddr = physAddr;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

static void _LatteAsyncCommands_cpuReadback(MPTR physAddr)
{
	std::vector<LatteTexture*> textures;
	LatteTC_LookupTexturesByPhysAddr(physAddr, textures);
	if (cemuLog_isLoggingEnabled(LogType::TextureReadback))
	{
		for (LatteTexture* tex : textures)
		{
			LatteTextureSliceMipInfo* smi = tex->sliceMipInfo + tex->GetSliceMipArrayIndex(0, 0);
			cemuLog_log(LogType::TextureReadback, "[TextureReadback-CPU] candidate {:08x} {}x{} pitch {} fmt {:04x} TM {} gpuWritten {} linear-mirrored {} lastDynamicUpdate {} lastCpuReadback {}", tex->physAddress, tex->width, tex->height, tex->pitch, (uint32)tex->format, (uint32)tex->tileMode, tex->isUpdatedOnGPU, tex->enableReadback, smi->lastDynamicUpdate, tex->lastCpuReadbackDynamicUpdate);
		}
		if (textures.empty())
			cemuLog_log(LogType::TextureReadback, "[TextureReadback-CPU] no cached texture at {:08x}", physAddr);
	}
	for (LatteTexture* tex : textures)
	{
		if (!tex->isUpdatedOnGPU || tex->isDepth || tex->enableReadback) // linear ones are mirrored already
			continue;
		if (tex->overwriteInfo.hasResolutionOverwrite || tex->dim != Latte::E_DIM::DIM_2D || !tex->baseView)
			continue;
		if (Latte::GetHWFormat(tex->format) != Latte::E_HWFMT::HWFMT_8_8_8_8) // what the RAM write-back handles for tiled textures
			continue;
		LatteTextureSliceMipInfo* sliceMipInfo = tex->sliceMipInfo + tex->GetSliceMipArrayIndex(0, 0);
		// only if the GPU wrote it since the last such readback: the same call after the CPU edits the data must not undo them
		if (sliceMipInfo->lastDynamicUpdate <= tex->lastCpuReadbackDynamicUpdate)
			continue;
		tex->lastCpuReadbackDynamicUpdate = sliceMipInfo->lastDynamicUpdate;
		cemuLog_log(LogType::TextureReadback, "[TextureReadback-CPU] PhysAddr {:08x} Res {}x{} Fmt {:04x} TM {}", tex->physAddress, tex->width, tex->height, (uint32)tex->format, (uint32)tex->tileMode);
		LatteTexture_UpdateDataToLatest(tex);
		LatteTextureReadback_StartTransfer(tex->baseView);
		LatteTextureReadback_UpdateFinishedTransfers(true);
	}
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_DELETE_SHADER;

	asyncCommand.deleteShader.shaderBaseHash = shaderBaseHash;
	asyncCommand.deleteShader.shaderAuxHash = shaderAuxHash;
	asyncCommand.deleteShader.shaderType = shaderType;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_TEXTURE_COPY;
	asyncCommand.textureCopy.src = src;
	asyncCommand.textureCopy.dst = dst;
	asyncCommand.textureCopy.rect = rect;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	while (LatteAsyncCommandQueue.empty() == false)
	{
		_mm_pause();
	}
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	// quick check if queue is empty (requires no lock)
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	if (LatteAsyncCommandQueue.empty())
		return;
	swl_gpuAsyncCommands.LockWrite();
	while (LatteAsyncCommandQueue.empty() == false)
	{
		// get first command in queue
		LatteAsyncCommand_t asyncCommand = LatteAsyncCommandQueue.front();
		swl_gpuAsyncCommands.UnlockWrite();
		if (asyncCommand.type == ASYNC_CMD_FORCE_TEXTURE_READBACK)
		{
			cemu_assert_debug(asyncCommand.forceTextureReadback.level == 0); // implement mip swizzle and verify
			LatteTextureView* textureView = LatteTC_GetTextureSliceViewOrTryCreate(asyncCommand.forceTextureReadback.physAddr, asyncCommand.forceTextureReadback.mipAddr, (Latte::E_GX2SURFFMT)asyncCommand.forceTextureReadback.format, asyncCommand.forceTextureReadback.tilemode, asyncCommand.forceTextureReadback.width, asyncCommand.forceTextureReadback.height, asyncCommand.forceTextureReadback.depth, asyncCommand.forceTextureReadback.pitch, 0, asyncCommand.forceTextureReadback.slice, asyncCommand.forceTextureReadback.level);
			if (textureView != nullptr)
			{
				LatteTexture_UpdateDataToLatest(textureView->baseTexture);
				// start transfer
				LatteTextureReadback_StartTransfer(textureView);
				// wait until finished
				LatteTextureReadback_UpdateFinishedTransfers(true);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Texture not found for readback");
			}
		}
		else if (asyncCommand.type == ASYNC_CMD_DELETE_SHADER)
		{
			LatteSHRC_RemoveFromCacheByHash(asyncCommand.deleteShader.shaderBaseHash, asyncCommand.deleteShader.shaderAuxHash, asyncCommand.deleteShader.shaderType);
		}
		else if (asyncCommand.type == ASYNC_CMD_TEXTURE_COPY)
		{
			LatteSurfaceCopy_copySurfaceNew(asyncCommand.textureCopy.src, asyncCommand.textureCopy.dst, asyncCommand.textureCopy.rect);
		}
		else if (asyncCommand.type == ASYNC_CMD_CPU_READBACK)
		{
			_LatteAsyncCommands_cpuReadback(asyncCommand.cpuReadback.physAddr);
		}
		else
		{
			cemu_assert_unimplemented();
		}
		swl_gpuAsyncCommands.LockWrite();
		LatteAsyncCommandQueue.pop();
	}
	swl_gpuAsyncCommands.UnlockWrite();
}
