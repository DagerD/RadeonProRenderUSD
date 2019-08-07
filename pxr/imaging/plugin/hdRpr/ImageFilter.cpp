#include "ImageFilter.h"

#include "RadeonProRender_CL.h"
#include "RadeonProRender_GL.h"
#include "RadeonImageFilters_cl.h"
#include "RadeonImageFilters_gl.h"

#include <GL/glew.h>

#include "pxr/base/tf/diagnostic.h"

#include <vector>
#include <cassert>
#include <exception>

PXR_NAMESPACE_OPEN_SCOPE

static bool HasGpuContext(rpr_creation_flags contextFlags)
{
#define GPU(x) RPR_CREATION_FLAGS_ENABLE_GPU##x

	rpr_creation_flags gpuMask = GPU(0) | GPU(1) | GPU(2) | GPU(3) | GPU(4) | GPU(5) | GPU(6) | GPU(7) |
		GPU(8) | GPU(9) | GPU(10) | GPU(11) | GPU(12) | GPU(13) | GPU(14) | GPU(15);

#undef GPU

	bool hasGpuContext = (contextFlags & gpuMask) != 0;

	return hasGpuContext;
}

static rpr_int GpuDeviceIdUsed(rpr_creation_flags contextFlags)
{
#define GPU(x) RPR_CREATION_FLAGS_ENABLE_GPU##x

	std::vector<rpr_int> gpu_ids;
	gpu_ids.reserve(16);
	gpu_ids.push_back(GPU(0));
	gpu_ids.push_back(GPU(1));
	gpu_ids.push_back(GPU(2));
	gpu_ids.push_back(GPU(3));
	gpu_ids.push_back(GPU(4));
	gpu_ids.push_back(GPU(5));
	gpu_ids.push_back(GPU(6));
	gpu_ids.push_back(GPU(7));
	gpu_ids.push_back(GPU(8));
	gpu_ids.push_back(GPU(9));
	gpu_ids.push_back(GPU(10));
	gpu_ids.push_back(GPU(11));
	gpu_ids.push_back(GPU(12));
	gpu_ids.push_back(GPU(13));
	gpu_ids.push_back(GPU(14));
	gpu_ids.push_back(GPU(15));
	
#undef GPU

	for (rpr_int i = 0; i < gpu_ids.size(); i++ )
	{	
		if ((contextFlags & gpu_ids[i]) != 0)
			return i;
	}
	
	return -1;
}

ImageFilter::ImageFilter(const rpr_context rprContext, std::uint32_t width, std::uint32_t height) :
	mWidth(width),
	mHeight(height),
	mIsCPUMode(false)
{
	rpr_creation_flags contextFlags = 0;
	rpr_int rprStatus = rprContextGetInfo(rprContext, RPR_CONTEXT_CREATION_FLAGS, sizeof(rpr_creation_flags), &contextFlags, nullptr);
	assert(RPR_SUCCESS == rprStatus);
	
	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get context parameters.");

	if (contextFlags & RPR_CREATION_FLAGS_ENABLE_METAL)
	{
		mRifContext.reset( new RifContextGPUMetal(rprContext) );
	}
	else if (HasGpuContext(contextFlags))
	{
		mRifContext.reset(new RifContextGPU(rprContext));
	}
	else
	{
		mRifContext.reset(new RifContextCPU(rprContext));
		mIsCPUMode = true;
	}
}

ImageFilter::~ImageFilter()
{
	mRifFilter->DetachFilter( mRifContext.get() );
}

void ImageFilter::CreateFilter()
{
	mRifFilter.reset(new RifFilterAIDenoise(mRifContext.get(), mIsCPUMode));
}

void ImageFilter::DeleteFilter()
{
	mRifFilter->DetachFilter( mRifContext.get() );
}

void ImageFilter::Resize(std::uint32_t w, std::uint32_t h)
{
	mWidth = w;
	mHeight = h;
}

void ImageFilter::SetInput(RifFilterInput inputId, const rpr_framebuffer rprFrameBuffer, const float sigma) const
{
	rif_image_desc desc = { mWidth, mHeight, 1, mWidth, mWidth * mHeight, 4, RIF_COMPONENT_TYPE_FLOAT32 };

	rif_image rifImage = mRifContext->CreateRifImage(rprFrameBuffer, desc);

	mRifFilter->AddInput(inputId, rifImage, rprFrameBuffer, sigma);
}

void ImageFilter::SetOutput(const rpr_framebuffer rprFrameBuffer) const
{
	rif_image_desc desc = { mWidth, mHeight, 1, mWidth, mWidth * mHeight, 4, RIF_COMPONENT_TYPE_FLOAT32 };

	rif_image rifImage = mRifContext->CreateRifImage(rprFrameBuffer, desc);

	mRifContext->SetOutput(rifImage);
}

void ImageFilter::SetOutputGlTexture(const unsigned int glTexture) const
{
	rif_image rifImage = mRifContext->CreateRifImageGl(glTexture);

	mRifContext->SetOutput(rifImage);
}

void ImageFilter::AddParam(std::string name, RifParam param) const
{
	mRifFilter->AddParam(name, param);
}

void ImageFilter::AttachFilter() const
{
	mRifFilter->AttachFilter( mRifContext.get() );
	mRifFilter->ApplyParameters();
}

void ImageFilter::Run() const
{
	mRifContext->UpdateInputs( mRifFilter.get() );

	rif_int rifStatus = rifContextExecuteCommandQueue(mRifContext->Context(), mRifContext->Queue(), nullptr, nullptr, nullptr);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to execute queue.");

    rifStatus = rifSyncronizeQueue(mRifContext->Queue());
    assert(RIF_SUCCESS == rifStatus);

    if (RIF_SUCCESS != rifStatus)
        throw std::runtime_error("RPR denoiser failed to synchronize queue.");
}

std::vector<float> ImageFilter::GetData() const
{
	void* output = nullptr;

	rif_int rifStatus = rifImageMap(mRifContext->Output(), RIF_IMAGE_MAP_READ, &output);
	assert(RIF_SUCCESS == rifStatus);
	assert(output != nullptr);

	if (RIF_SUCCESS != rifStatus || nullptr == output)
		throw std::runtime_error("RPR denoiser failed to map output data.");

	std::vector<float> floatData( (float*) output, ( (float*) output ) + mWidth * mHeight * 4 );

	rifStatus = rifImageUnmap(mRifContext->Output(), output);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to unmap output data.");

	return std::move(floatData);
}

RifContextWrapper::~RifContextWrapper()
{	
}

const rif_context RifContextWrapper::Context() const
{
	return mRifContextHandle;
}

const rif_command_queue RifContextWrapper::Queue() const
{
	return mRifCommandQueueHandle;
}

const rif_image RifContextWrapper::Output() const
{
	return mOutputRifImage;
}

void RifContextWrapper::SetOutput(const rif_image& img)
{
	mOutputRifImage = img;
}

std::vector<rpr_char> RifContextWrapper::GetRprCachePath(rpr_context rprContext) const
{
	size_t length;
	rpr_status rprStatus = rprContextGetInfo(rprContext, RPR_CONTEXT_CACHE_PATH, sizeof(size_t), nullptr, &length);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get cache path.");

	std::vector<rpr_char> path(length);
	rprStatus = rprContextGetInfo(rprContext, RPR_CONTEXT_CACHE_PATH, path.size(), &path[0], nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get cache path.");

	return std::move(path);
}



RifContextGPU::RifContextGPU(const rpr_context rprContext)
{
	int deviceCount = 0;

	rif_int rifStatus = rifGetDeviceCount(rifBackendApiType, &deviceCount);
	assert(RIF_SUCCESS == rifStatus);
	assert(deviceCount != 0);

	if (RIF_SUCCESS != rifStatus || 0 == deviceCount)
		throw std::runtime_error("RPR denoiser hasn't found compatible devices.");

	rpr_cl_context clContext;
	rpr_int rprStatus = rprContextGetInfo(rprContext, RPR_CL_CONTEXT, sizeof(rpr_cl_context), &clContext, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get CL device context.");

	rpr_cl_device clDevice;
	rprStatus = rprContextGetInfo(rprContext, RPR_CL_DEVICE, sizeof(rpr_cl_device), &clDevice, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get CL device.");

	rpr_cl_command_queue clCommandQueue;
	rprStatus = rprContextGetInfo(rprContext, RPR_CL_COMMAND_QUEUE, sizeof(rpr_cl_command_queue), &clCommandQueue, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get CL command queue.");

	std::vector<rpr_char> path = GetRprCachePath(rprContext);

	rifStatus = rifCreateContextFromOpenClContext(RIF_API_VERSION, clContext, clDevice, clCommandQueue, path.data(), &mRifContextHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF context.");

	rifStatus = rifContextCreateCommandQueue(mRifContextHandle, &mRifCommandQueueHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF command queue.");
}

RifContextGPU::~RifContextGPU()
{
}

rif_image RifContextGPU::CreateRifImage(const rpr_framebuffer rprFrameBuffer, const rif_image_desc& desc) const
{
	rif_image rifImage = nullptr;
	rpr_cl_mem clMem = nullptr;

	rpr_int rprStatus = rprFrameBufferGetInfo(rprFrameBuffer, RPR_CL_MEM_OBJECT, sizeof(rpr_cl_mem), &clMem, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get frame buffer info.");

	rif_int rifStatus = rifContextCreateImageFromOpenClMemory(mRifContextHandle , &desc, clMem, false, &rifImage);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to get frame buffer info.");

	return rifImage;
}

rif_image RifContextGPU::CreateRifImageGl(const unsigned int glTexture) const
{
	rif_image rifImage = nullptr;
	rif_int rifStatus = rifContextCreateImageFromOpenGlTexture(mRifContextHandle, GL_TEXTURE_2D, 0, glTexture, &rifImage);
	if (RIF_SUCCESS != rifStatus)
	{
		TF_CODING_ERROR("Fail to create rif image from OpenGL texture. Error code: %d", rifStatus);
		return NULL;
	}

	return rifImage;
}

void RifContextGPU::UpdateInputs(const RifFilterWrapper* rifFilter) const
{
	// image filter processes buffers directly in GPU mode
}



RifContextCPU::RifContextCPU(const rpr_context rprContext)
{
	int deviceCount = 0;
	rif_int rifStatus = rifGetDeviceCount(rifBackendApiType, &deviceCount);
	assert(RIF_SUCCESS == rifStatus);
	assert(deviceCount != 0);

	if (RIF_SUCCESS != rifStatus || 0 == deviceCount)
		throw std::runtime_error("RPR denoiser hasn't found compatible devices.");

	std::vector<rpr_char> path = GetRprCachePath(rprContext);

	rifStatus = rifCreateContext(RIF_API_VERSION, rifBackendApiType, 0, path.data(), &mRifContextHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF context.");

	rifStatus = rifContextCreateCommandQueue(mRifContextHandle, &mRifCommandQueueHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF command queue.");
}

RifContextCPU::~RifContextCPU()
{
}

rif_image RifContextCPU::CreateRifImage(const rpr_framebuffer rprFrameBuffer, const rif_image_desc& desc) const
{
	rif_image rifImage = nullptr;

	rif_int rifStatus = rifContextCreateImage(mRifContextHandle, &desc, nullptr, &rifImage);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF image.");

	return rifImage;
}

void RifContextCPU::UpdateInputs(const RifFilterWrapper* rifFilter) const
{
	// data have to be acquired from RPR framebuffers and moved to filter inputs

	for (const auto& input : rifFilter->mInputs)
	{
		const RifFilterWrapper::InputTraits& inputData = input.second;

		size_t sizeInBytes = 0;
		size_t retSize = 0;
		void* imageData = nullptr;

		// verify image size
		rif_int rifStatus = rifImageGetInfo(inputData.mRifImage, RIF_IMAGE_DATA_SIZEBYTE, sizeof(size_t), (void*) &sizeInBytes, &retSize);
		assert(RIF_SUCCESS == rifStatus);

		if (RIF_SUCCESS != rifStatus)
			throw std::runtime_error("RPR denoiser failed to get RIF image info.");

		size_t fbSize;
		rpr_int rprStatus = rprFrameBufferGetInfo(inputData.mRprFrameBuffer, RPR_FRAMEBUFFER_DATA, 0, NULL, &fbSize);
		assert(RPR_SUCCESS == rprStatus);

		if (RPR_SUCCESS != rprStatus)
			throw std::runtime_error("RPR denoiser failed to acquire frame buffer info.");

		assert(sizeInBytes == fbSize);
	
		if (sizeInBytes != fbSize)
			throw std::runtime_error("RPR denoiser failed to match RIF image and frame buffer sizes.");

		// resolve framebuffer data to rif image
		rifStatus = rifImageMap(inputData.mRifImage, RIF_IMAGE_MAP_WRITE, &imageData);
		assert(RIF_SUCCESS == rifStatus);

		if (RIF_SUCCESS != rifStatus)
			throw std::runtime_error("RPR denoiser failed to acquire RIF image.");

		rprStatus = rprFrameBufferGetInfo(inputData.mRprFrameBuffer, RPR_FRAMEBUFFER_DATA, fbSize, imageData, NULL);
		assert(RPR_SUCCESS == rprStatus);

		// try to unmap at first, then raise a possible error

		rifStatus = rifImageUnmap(inputData.mRifImage, imageData);
		assert(RIF_SUCCESS == rifStatus);

		if (RPR_SUCCESS != rprStatus)
			throw std::runtime_error("RPR denoiser failed to get data from frame buffer.");

		if (RIF_SUCCESS != rifStatus)
			throw std::runtime_error("RPR denoiser failed to unmap output data.");
	}
}



RifContextGPUMetal::RifContextGPUMetal(const rpr_context rprContext)
{
	int deviceCount = 0;
	rif_int rifStatus = rifGetDeviceCount(rifBackendApiType, &deviceCount);
	assert(RIF_SUCCESS == rifStatus);
	assert(deviceCount != 0);

	if (RIF_SUCCESS != rifStatus || 0 == deviceCount)
		throw std::runtime_error("RPR denoiser hasn't found compatible devices.");
	
	rpr_creation_flags contextFlags = 0;
	rpr_int rprStatus = rprContextGetInfo(rprContext, RPR_CONTEXT_CREATION_FLAGS, sizeof(rpr_creation_flags), &contextFlags, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	std::vector<rpr_char> path = GetRprCachePath(rprContext);

	// we find the active gpu from the rpr contextFlags and then use that to create the rif context
	rifStatus = rifCreateContext(RIF_API_VERSION, rifBackendApiType, GpuDeviceIdUsed(contextFlags), path.data(), &mRifContextHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF context.");
    
	rifStatus = rifContextCreateCommandQueue(mRifContextHandle, &mRifCommandQueueHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create RIF command queue.");
}

RifContextGPUMetal::~RifContextGPUMetal()
{
}

rif_image RifContextGPUMetal::CreateRifImage(const rpr_framebuffer rprFrameBuffer, const rif_image_desc& desc) const
{
	rif_image rifImage = nullptr;
	rpr_cl_mem clMem = nullptr;

	rpr_int rprStatus = rprFrameBufferGetInfo(rprFrameBuffer, RPR_CL_MEM_OBJECT, sizeof(rpr_cl_mem), &clMem, nullptr);
	assert(RPR_SUCCESS == rprStatus);

	if (RPR_SUCCESS != rprStatus)
		throw std::runtime_error("RPR denoiser failed to get frame buffer info.");

	rif_int rifStatus = rifContextCreateImageFromOpenClMemory(mRifContextHandle , &desc, clMem, false, &rifImage);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to get frame buffer info.");

	return rifImage;
}

void RifContextGPUMetal::UpdateInputs(const RifFilterWrapper* rifFilter) const
{
	// image filter processes buffers directly in GPU mode
}



RifFilterWrapper::~RifFilterWrapper()
{
	rif_int rifStatus = RIF_SUCCESS;

	for (const auto& input : mInputs)
	{
		rifStatus = rifObjectDelete(input.second.mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	for (const rif_image& auxImage : mAuxImages)
	{
		rifStatus = rifObjectDelete(auxImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	for (const rif_image_filter& auxFilter : mAuxFilters)
	{
		rifStatus = rifObjectDelete(auxFilter);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (mRifImageFilterHandle != nullptr)
	{
		rifStatus = rifObjectDelete(mRifImageFilterHandle);
		assert(RIF_SUCCESS == rifStatus);
	}
}

void RifFilterWrapper::AddInput(RifFilterInput inputId, rif_image rifImage, const rpr_framebuffer rprFrameBuffer, float sigma)
{
	mInputs[inputId] = { rifImage, rprFrameBuffer, sigma };
}

void RifFilterWrapper::AddParam(std::string name, RifParam param)
{
	mParams[name] = param;
}

void RifFilterWrapper::DetachFilter(const RifContextWrapper* rifContext) noexcept
{
	rif_int rifStatus = RIF_SUCCESS;

	for (const rif_image_filter& auxFilter : mAuxFilters)
	{
		rifStatus = rifCommandQueueDetachImageFilter(rifContext->Queue(), auxFilter);
		assert(RIF_SUCCESS == rifStatus);
	}

	rifStatus = rifCommandQueueDetachImageFilter(rifContext->Queue(), mRifImageFilterHandle);
	assert(RIF_SUCCESS == rifStatus);
}

void RifFilterWrapper::SetupVarianceImageFilter(const rif_image_filter inputFilter, const rif_image outVarianceImage) const
{
	rif_int rifStatus = rifImageFilterSetParameterImage(inputFilter, "positionsImg", mInputs.at(RifWorldCoordinate).mRifImage);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(inputFilter, "normalsImg", mInputs.at(RifNormal).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(inputFilter, "meshIdsImg", mInputs.at(RifObjectId).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(inputFilter, "outVarianceImg", outVarianceImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to setup variance filter.");
}

void RifFilterWrapper::ApplyParameters() const
{
	rif_int rifStatus = RIF_SUCCESS;

	for (const auto& param : mParams)
	{
		switch (param.second.mType)
		{
		case RifParamType::RifInt:
			rifStatus = rifImageFilterSetParameter1u(mRifImageFilterHandle, param.first.c_str(), param.second.mData.i);
			break;

		case RifParamType::RifFloat:
			rifStatus = rifImageFilterSetParameter1f(mRifImageFilterHandle, param.first.c_str(), param.second.mData.f);
			break;
		}

		assert(RIF_SUCCESS == rifStatus);

		if (RIF_SUCCESS != rifStatus)
			throw std::runtime_error("RPR denoiser failed to apply parameter.");
	}
}



RifFilterAIDenoise::RifFilterAIDenoise(const RifContextWrapper* rifContext, bool isCPUMode)
{
    rif_image_filter_type rifFilterType = RIF_IMAGE_FILTER_OPENIMAGE_DENOISE;
#ifndef __APPLE__
    if (!isCPUMode) {
        rifFilterType = RIF_IMAGE_FILTER_AI_DENOISE;
    }
#endif
	rif_int rifStatus = rifContextCreateImageFilter(rifContext->Context(), rifFilterType, &mRifImageFilterHandle);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create AI denoise filter.");

    if (rifFilterType == RIF_IMAGE_FILTER_AI_DENOISE) {
        rifStatus = rifImageFilterSetParameter1u(mRifImageFilterHandle, "useHDR", 1);
        if (RIF_SUCCESS != rifStatus)
            throw std::runtime_error("RPR denoiser failed to set filter \"usdHDR\" parameter.");
    }

    // TODO: set correct model path
    rifStatus = rifImageFilterSetParameterString(mRifImageFilterHandle, "modelPath", "../models");
    if (RIF_SUCCESS != rifStatus)
        throw std::runtime_error("RPR denoiser failed to set filter \"modelPath\" parameter.");

	// auxillary filters
	mAuxFilters.resize(AuxFilterMax, nullptr);

	rifStatus = rifContextCreateImageFilter(rifContext->Context(), RIF_IMAGE_FILTER_REMAP_RANGE, &mAuxFilters[RemapNormalFilter]);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create auxillary filter.");

	rifStatus = rifContextCreateImageFilter(rifContext->Context(), RIF_IMAGE_FILTER_REMAP_RANGE, &mAuxFilters[RemapDepthFilter]);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to create auxillary filter.");
}

void RifFilterAIDenoise::AttachFilter(const RifContextWrapper* rifContext)
{
	// setup inputs
	rif_int rifStatus = rifImageFilterSetParameterImage(mRifImageFilterHandle, "normalsImg", mInputs.at(RifNormal).mRifImage);
	assert(RIF_SUCCESS == rifStatus);

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(mRifImageFilterHandle, "depthImg", mInputs.at(RifDepth).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(mRifImageFilterHandle, "colorImg", mInputs.at(RifColor).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameterImage(mRifImageFilterHandle, "albedoImg", mInputs.at(RifAlbedo).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}
	
	// setup remapping filters
	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameter1f(mAuxFilters[RemapNormalFilter], "dstLo", -1.0f);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameter1f(mAuxFilters[RemapNormalFilter], "dstHi", +1.0f);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameter1f(mAuxFilters[RemapDepthFilter], "dstLo", 0.0f);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifImageFilterSetParameter1f(mAuxFilters[RemapDepthFilter], "dstHi", 1.0f);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to apply parameter.");
	
	// attach filters

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifCommandQueueAttachImageFilter(rifContext->Queue(), mAuxFilters[RemapNormalFilter], mInputs.at(RifNormal).mRifImage, mInputs.at(RifNormal).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifCommandQueueAttachImageFilter(rifContext->Queue(), mAuxFilters[RemapDepthFilter], mInputs.at(RifDepth).mRifImage, mInputs.at(RifDepth).mRifImage);
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS == rifStatus)
	{
		rifStatus = rifCommandQueueAttachImageFilter(rifContext->Queue(), mRifImageFilterHandle, mInputs.at(RifColor).mRifImage, rifContext->Output());
		assert(RIF_SUCCESS == rifStatus);
	}

	if (RIF_SUCCESS != rifStatus)
		throw std::runtime_error("RPR denoiser failed to attach filter to queue.");
}

PXR_NAMESPACE_CLOSE_SCOPE
