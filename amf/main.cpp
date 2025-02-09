#pragma push_macro("_WIN32")
#pragma push_macro("WIN32")
#undef _WIN32
#undef WIN32
#include "tracy/Tracy.hpp"
#include <vulkan/vulkan_core.h>
#pragma pop_macro("WIN32")
#pragma pop_macro("_WIN32")

#include <unistd.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cassert>
#include <fcntl.h>
#include <dlfcn.h>
#include <memory>
#include "wine/debug.h"
#include "winecompat.h"
#include "iconv.h"

#pragma push_macro("_WIN32")
#undef _WIN32

#include <d3d11_3.h> // We need this for the interop
#include "dxvk-interop.h"
#include <AMF/core/Factory.h>
#include <AMF/core/VulkanAMF.h>
#include <AMF/core/Data.h>

#pragma pop_macro("_WIN32")

#define DLL_EXPORT extern "C" MSABI
#define DLL_IMPORT extern "C"

#define MSABI __attribute__((ms_abi))

#define STUB() \
do { \
	WINE_FIXME("Stub %s\n", __PRETTY_FUNCTION__); \
}while(0)

WINE_DEFAULT_DEBUG_CHANNEL(amf);

#define VK_CHECK_RESULT(f)																				\
{																										\
	VkResult res = (f);																					\
	if (res != VK_SUCCESS)																				\
	{																									\
		WINE_ERR("Fatal : VkResult\n");                                                                 \
		assert(res == VK_SUCCESS);																		\
	}																									\
}

#define VK_PROCS                \
	X(vkGetImageMemoryRequirements) \
	X(vkCmdBlitImage) \
	X(vkCmdCopyImage) \
	X(vkCreateImage) \
	X(vkCreateFence) \
	X(vkDestroyImage) \
	X(vkFreeMemory) \
	X(vkCmdPipelineBarrier) \
	X(vkEndCommandBuffer) \
	X(vkQueueSubmit) \
	X(vkWaitForFences) \
	X(vkDestroyFence) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkGetImageSubresourceLayout) \
	X(vkFreeCommandBuffers) \
	X(vkAllocateCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkCreateCommandPool) \
	X(vkBindImageMemory) \
	X(vkAllocateMemory) \

PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;

void saveScreenshot(FILE *outFile, VkInstance instance, VkDevice device, VkQueue queue, uint32_t queueFamily, VkPhysicalDevice physicalDevice, VkFormat colorFormat, VkImage srcImage, uint32_t width, uint32_t height, VkImageTiling tiling, VkDeviceMemory srcImageMemory, VkDeviceSize srcImageSize) {
	PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)p_vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");

#define X(proc) PFN_##proc p_##proc = (PFN_##proc)p_vkGetDeviceProcAddr(device, #proc);
	VK_PROCS
#undef X

	PFN_vkGetPhysicalDeviceFormatProperties p_vkGetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties");
	PFN_vkGetPhysicalDeviceMemoryProperties p_vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)p_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");

	// Check blit support for source and destination
	VkFormatProperties formatProps;
	WINE_TRACE("Vulkan handles after unwrap %p %p %p\n", device, queue, physicalDevice);

	// Create the linear tiled destination image to copy to and to read the memory from
	VkImageCreateInfo imageCreateCI = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
	// Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would differ
	imageCreateCI.format = VK_FORMAT_G16_B16R16_2PLANE_420_UNORM;
	imageCreateCI.extent.width = width;
	imageCreateCI.extent.height = height;
	imageCreateCI.extent.depth = 1;
	imageCreateCI.arrayLayers = 1;
	imageCreateCI.mipLevels = 1;
	imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
	imageCreateCI.tiling = VK_IMAGE_TILING_LINEAR;
	imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	// Create the image
	VkImage dstImage;
	VK_CHECK_RESULT(p_vkCreateImage(device, &imageCreateCI, nullptr, &dstImage));
	// Create memory to back up the image
	VkMemoryRequirements memRequirements;
	VkMemoryAllocateInfo memAllocInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkDeviceMemory dstImageMemory;
	p_vkGetImageMemoryRequirements(device, dstImage, &memRequirements);
	memAllocInfo.allocationSize = memRequirements.size;
	// Memory must be host visible to copy from
	VkPhysicalDeviceMemoryProperties memoryProperties;
	p_vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

	uint32_t typeBits = memRequirements.memoryTypeBits;
	uint32_t properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
		if ((typeBits & 1) == 1 && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			memAllocInfo.memoryTypeIndex = i;
			goto foundProp;
		}
		typeBits >>= 1;
	}
	WINE_ERR("Could not find a matching memory type\n");
	return;
foundProp:

	VK_CHECK_RESULT(p_vkAllocateMemory(device, &memAllocInfo, nullptr, &dstImageMemory));
	VK_CHECK_RESULT(p_vkBindImageMemory(device, dstImage, dstImageMemory, 0));

	// Do the actual blit from the swapchain image to our host visible destination image
	VkCommandPoolCreateInfo cmdPoolCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queueFamily,
	};
	VkCommandPool cmdPool;
	VK_CHECK_RESULT(p_vkCreateCommandPool(device, &cmdPoolCreateInfo, nullptr, &cmdPool));

	VkCommandBufferAllocateInfo cmdBufAllocateInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cmdPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer copyCmd;
	VK_CHECK_RESULT(p_vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &copyCmd));
	VkCommandBufferBeginInfo cmdBufInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	VK_CHECK_RESULT(p_vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

	// Transition destination image to transfer destination layout
	{
		VkImageMemoryBarrier imageMemoryBarrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = dstImage,
			.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
		};
		p_vkCmdPipelineBarrier(
			copyCmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &imageMemoryBarrier
		);
	}

	// Transition swapchain image from present to transfer source layout
	{
		VkImageMemoryBarrier imageMemoryBarrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = srcImage,
			.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
		};
		p_vkCmdPipelineBarrier(
			copyCmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &imageMemoryBarrier
		);
	}

	// Otherwise use image copy (requires us to manually flip components)
	VkImageCopy imageCopyRegion[] = {
		{
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
				.layerCount = 1,
			},
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
				.layerCount = 1,
			},
			.extent = {
				.width = width,
				.height = height,
				.depth = 1,
			}
		},
		{
			.srcSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
				.layerCount = 1,
			},
			.dstSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
				.layerCount = 1,
			},
			.extent = {
				.width = width/2,
				.height = height/2,
				.depth = 1,
			}
		}
	};

	// Issue the copy command
	p_vkCmdCopyImage(
		copyCmd,
		srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		2,
		imageCopyRegion);

	// Transition destination image to general layout, which is the required layout for mapping the image memory later on
	{
		VkImageMemoryBarrier imageMemoryBarrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = dstImage,
			.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
		};
		p_vkCmdPipelineBarrier(
			copyCmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &imageMemoryBarrier
		);
	}

	// Transition back the swap chain image after the blit is done
	{
		VkImageMemoryBarrier imageMemoryBarrier = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = srcImage,
			.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
		};
		p_vkCmdPipelineBarrier(
			copyCmd,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &imageMemoryBarrier
		);
	}

	VK_CHECK_RESULT(p_vkEndCommandBuffer(copyCmd));

	VkSubmitInfo submitInfo = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &copyCmd,
	};
	// Create fence to ensure that the command buffer has finished executing
	VkFenceCreateInfo fenceInfo = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = 0,
	};
	VkFence fence;
	VK_CHECK_RESULT(p_vkCreateFence(device, &fenceInfo, nullptr, &fence));
	// Submit to the queue
	VK_CHECK_RESULT(p_vkQueueSubmit(queue, 1, &submitInfo, fence));
	// Wait for the fence to signal that command buffer has finished executing
	VK_CHECK_RESULT(p_vkWaitForFences(device, 1, &fence, VK_TRUE, 100000000000));
	p_vkDestroyFence(device, fence, nullptr);
	p_vkFreeCommandBuffers(device, cmdPool, 1, &copyCmd);

	// Map image memory so we can start copying from it
	const char* data;
	VK_CHECK_RESULT(p_vkMapMemory(device, dstImageMemory, 0, VK_WHOLE_SIZE, 0, (void**)&data));

	/* { */
	/* 	// Get layout of the image (including row pitch) */
	/* 	VkImageSubresource subResource { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0 }; */
	/* 	VkSubresourceLayout subResourceLayout; */
	/* 	p_vkGetImageSubresourceLayout(device, dstImage, &subResource, &subResourceLayout); */
	/* 	const char* cursor = data + subResourceLayout.offset; */
	/* 	WINE_TRACE("Plane %d@%ld pitch %ld\n", 0, subResourceLayout.offset, subResourceLayout.rowPitch); */

	/* 	// ppm binary pixel data */
	/* 	for (uint32_t y = 0; y < height; y++) { */
	/* 		fwrite(cursor, 1, width*2, outFile); */
	/* 		cursor += subResourceLayout.rowPitch; */
	/* 	} */
	/* } */

	/* { */
	/* 	// Get layout of the image (including row pitch) */
	/* 	VkImageSubresource subResource { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0 }; */
	/* 	VkSubresourceLayout subResourceLayout; */
	/* 	p_vkGetImageSubresourceLayout(device, dstImage, &subResource, &subResourceLayout); */
	/* 	const char* cursor = data + subResourceLayout.offset; */
	/* 	WINE_TRACE("Plane %d@%ld pitch %ld\n", 1, subResourceLayout.offset, subResourceLayout.rowPitch); */

	/* 	// ppm binary pixel data */
	/* 	for (uint32_t y = 0; y < height/2; y++) { */
	/* 		fwrite(cursor, 1, width*2, outFile); */
	/* 		cursor += subResourceLayout.rowPitch; */
	/* 	} */
	/* } */

	/* const char* data; */
	/* VK_CHECK_RESULT(p_vkMapMemory(device, srcImageMemory, 0, VK_WHOLE_SIZE, 0, (void**)&data)); */
	/* fwrite(data, 1, srcImageSize, outFile); */

	WINE_TRACE("Screenshot saved\n");

	// Clean up resources
	p_vkUnmapMemory(device, dstImageMemory);
	p_vkFreeMemory(device, dstImageMemory, nullptr);
	p_vkDestroyImage(device, dstImage, nullptr);
}

// This only works if we assume str is actually ASCII. It asserts on that
// though.
char32_t* winWStrToLinux(const char16_t *str) {
	size_t len = 0; //Includes trailing null
	do {
		// Check that all characters are in the ASCII set
		assert((str[len] & 0xFF00) == 0);
	} while(str[len++] != 0);

	char32_t *buf = (char32_t*)malloc(len * sizeof(char32_t));
	
	while((*(buf++) = (char32_t)*(str++)) != 0);
	return (char32_t*)(buf - (len));
}

char16_t* linuxWStrToWin(const char32_t *str) {
	size_t len = 0; //Includes trailing null
	do {
		// Check that all characters are in the ASCII set
		assert((str[len] & 0xFFFFFF00) == 0);
	} while(str[len++] != 0);

	char16_t *buf = (char16_t*)malloc(len * sizeof(char16_t));
	
	while((*(buf++) = (char16_t)*(str++)) != 0);
	return (char16_t*)(buf - (len));
}

class AMFBufferImpl;

class AMFInterfaceImpl {
	std::shared_ptr<amf::AMFInterface> inner;

public:
	AMF_DECLARE_IID(0x9d872f34, 0x90dc, 0x4b93, 0xb6, 0xb2, 0x6c, 0xa3, 0x7c, 0x85, 0x25, 0xdb)

	AMFInterfaceImpl(std::shared_ptr<amf::AMFInterface> inner) : inner(inner) {};

	MSABI virtual amf_long Acquire();
	MSABI virtual amf_long Release();
	MSABI virtual AMF_RESULT QueryInterface(const amf::AMFGuid& interfaceID, void** ppInterface);
};

class AMFPropertyStorageImpl : AMFInterfaceImpl {
	std::shared_ptr<amf::AMFPropertyStorage> inner;

public:
	AMF_DECLARE_IID(0xc7cec05b, 0xcfb9, 0x48af, 0xac, 0xe3, 0xf6, 0x8d, 0xf8, 0x39, 0x5f, 0xe3)

	AMFPropertyStorageImpl(std::shared_ptr<amf::AMFPropertyStorage> inner) : inner(inner), AMFInterfaceImpl(inner) {};

	MSABI virtual AMF_RESULT SetProperty(const char16_t* name, amf::AMFVariantStruct value);
	MSABI virtual AMF_RESULT GetProperty(const char16_t* name, amf::AMFVariantStruct* pValue) const;

	MSABI virtual amf_bool HasProperty(const char16_t* name) const;
	MSABI virtual amf_size GetPropertyCount() const;
	MSABI virtual AMF_RESULT GetPropertyAt(amf_size index, char16_t* name, amf_size nameSize, amf::AMFVariantStruct* pValue) const;

	MSABI virtual AMF_RESULT Clear();
	MSABI virtual AMF_RESULT AddTo(amf::AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep) const;
	MSABI virtual AMF_RESULT CopyTo(amf::AMFPropertyStorage* pDest, amf_bool deep) const;

	MSABI virtual void AddObserver(amf::AMFPropertyStorageObserver* pObserver);
	MSABI virtual void RemoveObserver(amf::AMFPropertyStorageObserver* pObserver);
};

class AMFDataImpl : AMFPropertyStorageImpl {

public:
	AMF_DECLARE_IID(0xa1159bf6, 0x9104, 0x4107, 0x8e, 0xaa, 0xc5, 0x3d, 0x5d, 0xba, 0xc5, 0x11)
	std::shared_ptr<amf::AMFData> inner;

	AMFDataImpl(std::shared_ptr<amf::AMFData> inner) : inner(inner), AMFPropertyStorageImpl(inner) {};

	MSABI virtual amf::AMF_MEMORY_TYPE GetMemoryType();

	MSABI virtual AMF_RESULT Duplicate(amf::AMF_MEMORY_TYPE type, AMFDataImpl** ppData);
	MSABI virtual AMF_RESULT Convert(amf::AMF_MEMORY_TYPE type);
	MSABI virtual AMF_RESULT Interop(amf::AMF_MEMORY_TYPE type);

	MSABI virtual amf::AMF_DATA_TYPE GetDataType();

	MSABI virtual amf_bool IsReusable();

	MSABI virtual void SetPts(amf_pts pts);
	MSABI virtual amf_pts GetPts();
	MSABI virtual void SetDuration(amf_pts duration);
	MSABI virtual amf_pts GetDuration();
};

class AMFSurfaceImpl : AMFDataImpl {

public:
	AMF_DECLARE_IID(0x3075dbe3, 0x8718, 0x4cfa, 0x86, 0xfb, 0x21, 0x14, 0xc0, 0xa5, 0xa4, 0x51)
	std::shared_ptr<amf::AMFSurface> inner;
	std::unique_ptr<amf::AMFVulkanSurface> surface;
	std::unique_ptr<ID3D11Texture2D> d3dTexture;

	AMFSurfaceImpl(std::shared_ptr<amf::AMFSurface> inner, std::unique_ptr<amf::AMFVulkanSurface> surface, std::unique_ptr<ID3D11Texture2D> interop) : inner(inner), AMFDataImpl(inner), surface(std::move(surface)), d3dTexture(std::move(interop)) {};

	MSABI virtual amf::AMF_SURFACE_FORMAT GetFormat();

	MSABI virtual amf_size GetPlanesCount();
	MSABI virtual amf::AMFPlane* GetPlaneAt(amf_size index);
	MSABI virtual amf::AMFPlane* GetPlane(amf::AMF_PLANE_TYPE type);

	MSABI virtual amf::AMF_FRAME_TYPE GetFrameType();
	MSABI virtual void SetFrameType(amf::AMF_FRAME_TYPE type);

	MSABI virtual AMF_RESULT SetCrop(amf_int32 x,amf_int32 y, amf_int32 width, amf_int32 height);
	MSABI virtual AMF_RESULT CopySurfaceRegion(amf::AMFSurface* pDest, amf_int32 dstX, amf_int32 dstY, amf_int32 srcX, amf_int32 srcY, amf_int32 width, amf_int32 height);

	MSABI virtual void AddObserver(amf::AMFSurfaceObserver* pObserver);
	MSABI virtual void RemoveObserver(amf::AMFSurfaceObserver* pObserver);
};

class AMFContextImpl : AMFPropertyStorageImpl {
	std::unique_ptr<amf::AMFVulkanDevice> device;
public:
	// This is a little hack
	std::shared_ptr<amf::AMFContext1> inner;

	AMFContextImpl(std::shared_ptr<amf::AMFContext1> inner) : inner(inner), AMFPropertyStorageImpl(inner) {};

	AMF_DECLARE_IID(0xd9e9f868, 0x6220, 0x44c6, 0xa2, 0x2f, 0x7c, 0xd6, 0xda, 0xc6, 0x86, 0x46)

	// Cleanup
	MSABI virtual AMF_RESULT Terminate();

	// DX9
	MSABI virtual AMF_RESULT InitDX9(void* pDX9Device);
	MSABI virtual void* GetDX9Device(amf::AMF_DX_VERSION dxVersionRequired = amf::AMF_DX9);
	MSABI virtual AMF_RESULT LockDX9();
	MSABI virtual AMF_RESULT UnlockDX9();
	class AMFDX9Locker;

	// DX11
	MSABI virtual AMF_RESULT InitDX11(void* pDX11Device, amf::AMF_DX_VERSION dxVersionRequired = amf::AMF_DX11_0);
	MSABI virtual void* GetDX11Device(amf::AMF_DX_VERSION dxVersionRequired = amf::AMF_DX11_0);
	MSABI virtual AMF_RESULT LockDX11();
	MSABI virtual AMF_RESULT UnlockDX11();
	class AMFDX11Locker;

	// OpenCL
	MSABI virtual AMF_RESULT InitOpenCL(void* pCommandQueue = NULL);
	MSABI virtual void* GetOpenCLContext();
	MSABI virtual void* GetOpenCLCommandQueue();
	MSABI virtual void* GetOpenCLDeviceID();
	MSABI virtual AMF_RESULT GetOpenCLComputeFactory(amf::AMFComputeFactory **ppFactory); // advanced compute - multiple queries
	MSABI virtual AMF_RESULT InitOpenCLEx(amf::AMFComputeDevice *pDevice);
	MSABI virtual AMF_RESULT LockOpenCL();
	MSABI virtual AMF_RESULT UnlockOpenCL();
	class AMFOpenCLLocker;

	// OpenGL
	MSABI virtual AMF_RESULT InitOpenGL(amf_handle hOpenGLContext, amf_handle hWindow, amf_handle hDC);
	MSABI virtual amf_handle GetOpenGLContext();
	MSABI virtual amf_handle GetOpenGLDrawable();
	MSABI virtual AMF_RESULT LockOpenGL();
	MSABI virtual AMF_RESULT UnlockOpenGL();
	class AMFOpenGLLocker;

	// XV - Linux
	MSABI virtual AMF_RESULT InitXV(void* pXVDevice);
	MSABI virtual void* GetXVDevice();
	MSABI virtual AMF_RESULT LockXV();
	MSABI virtual AMF_RESULT UnlockXV();
	class AMFXVLocker;

	// Gralloc - Android
	MSABI virtual AMF_RESULT InitGralloc(void* pGrallocDevice);
	MSABI virtual void* GetGrallocDevice();
	MSABI virtual AMF_RESULT LockGralloc();
	MSABI virtual AMF_RESULT UnlockGralloc();
	class AMFGrallocLocker;

	// Allocation
	MSABI virtual AMF_RESULT AllocBuffer(amf::AMF_MEMORY_TYPE type, amf_size size, amf::AMFBuffer** ppBuffer);
	MSABI virtual AMF_RESULT AllocSurface(amf::AMF_MEMORY_TYPE type, amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf::AMFSurface** ppSurface);
	MSABI virtual AMF_RESULT AllocAudioBuffer(amf::AMF_MEMORY_TYPE type, amf::AMF_AUDIO_FORMAT format, amf_int32 samples, amf_int32 sampleRate, amf_int32 channels, 
												amf::AMFAudioBuffer** ppAudioBuffer);

	// Wrap existing objects
	MSABI virtual AMF_RESULT CreateBufferFromHostNative(void* pHostBuffer, amf_size size, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromHostNative(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf_int32 hPitch, amf_int32 vPitch, void* pData, 
												 amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromDX9Native(void* pDX9Surface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromDX11Native(void* pDX11Surface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromOpenGLNative(amf::AMF_SURFACE_FORMAT format, amf_handle hGLTextureID, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromGrallocNative(amf_handle hGrallocSurface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateSurfaceFromOpenCLNative(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, void** pClPlanes, 
												 amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateBufferFromOpenCLNative(void* pCLBuffer, amf_size size, amf::AMFBuffer** ppBuffer);

	// Access to AMFCompute interface - AMF_MEMORY_OPENCL, AMF_MEMORY_COMPUTE_FOR_DX9, AMF_MEMORY_COMPUTE_FOR_DX11 are currently supported
	MSABI virtual AMF_RESULT GetCompute(amf::AMF_MEMORY_TYPE eMemType, amf::AMFCompute** ppCompute);

	MSABI virtual AMF_RESULT CreateBufferFromDX11Native(void* pHostBuffer, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver);

	MSABI virtual AMF_RESULT AllocBufferEx(amf::AMF_MEMORY_TYPE type, amf_size size, amf::AMF_BUFFER_USAGE usage, amf::AMF_MEMORY_CPU_ACCESS access, amf::AMFBuffer** ppBuffer);
	MSABI virtual AMF_RESULT AllocSurfaceEx(amf::AMF_MEMORY_TYPE type, amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf::AMF_SURFACE_USAGE usage, amf::AMF_MEMORY_CPU_ACCESS access, amf::AMFSurface** ppSurface);

	// Vulkan - Windows, Linux
	MSABI virtual AMF_RESULT InitVulkan(void* pVulkanDevice);
	MSABI virtual void* GetVulkanDevice();
	MSABI virtual AMF_RESULT LockVulkan();
	MSABI virtual AMF_RESULT UnlockVulkan();

	MSABI virtual AMF_RESULT CreateSurfaceFromVulkanNative(void* pVulkanImage, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver);
	MSABI virtual AMF_RESULT CreateBufferFromVulkanNative(void* pVulkanBuffer, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver);
	MSABI virtual AMF_RESULT GetVulkanDeviceExtensions(amf_size *pCount, const char **ppExtensions);
	class AMFVulkanLocker;
};

class AMFIOCapsImpl : AMFInterfaceImpl {
	std::shared_ptr<amf::AMFIOCaps> inner;

public:
	AMFIOCapsImpl(std::shared_ptr<amf::AMFIOCaps> inner) : inner(inner), AMFInterfaceImpl(inner) {};

	MSABI virtual void GetWidthRange(amf_int32* minWidth, amf_int32* maxWidth) const;
	MSABI virtual void GetHeightRange(amf_int32* minHeight, amf_int32* maxHeight) const;
	MSABI virtual amf_int32 GetVertAlign() const;
	MSABI virtual amf_int32 GetNumOfFormats() const;
	MSABI virtual AMF_RESULT GetFormatAt(amf_int32 index, amf::AMF_SURFACE_FORMAT* format, amf_bool* native) const;
	MSABI virtual amf_int32 GetNumOfMemoryTypes() const;
	MSABI virtual AMF_RESULT GetMemoryTypeAt(amf_int32 index, amf::AMF_MEMORY_TYPE* memType, amf_bool* native) const;
	MSABI virtual amf_bool IsInterlacedSupported() const;
};

class AMFCapsImpl : AMFPropertyStorageImpl {
	std::shared_ptr<amf::AMFCaps> inner;

public:
	AMFCapsImpl(std::shared_ptr<amf::AMFCaps> inner) : inner(inner), AMFPropertyStorageImpl(inner) {};

	MSABI virtual amf::AMF_ACCELERATION_TYPE GetAccelerationType() const;
	MSABI virtual AMF_RESULT GetInputCaps(amf::AMFIOCaps** input);
	MSABI virtual AMF_RESULT GetOutputCaps(amf::AMFIOCaps** output);
};

class AMFComponentImpl : AMFPropertyStorageImpl {
	std::shared_ptr<amf::AMFComponent> inner;
	FILE* videofile;
	amf::AMFTrace *trace;
	
public:
	AMFComponentImpl(std::shared_ptr<amf::AMFComponent> inner, amf::AMFTrace *trace) : inner(inner), AMFPropertyStorageImpl(inner), trace(trace) {
		char filename[256];
		int fd;
		for(uint32_t i = 0; i < 16; i++) {
			snprintf(filename, 256, "video_%d.mkv", i);
			fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
			if(fd < 0) continue;
			break;
		}
		assert(fd >= 0);
		videofile = fdopen(fd, "wb");
	};

	AMF_DECLARE_IID(0x8b51e5e4, 0x455d, 0x4034, 0xa7, 0x46, 0xde, 0x1b, 0xed, 0xc3, 0xc4, 0x6)

	// AMFPropertyEx
	MSABI virtual amf_size GetPropertiesInfoCount() const;
	MSABI virtual AMF_RESULT GetPropertyInfo(amf_size index, const amf::AMFPropertyInfo** ppInfo) const;
	MSABI virtual AMF_RESULT GetPropertyInfo(const wchar_t* name, const amf::AMFPropertyInfo** ppInfo) const;
	MSABI virtual AMF_RESULT ValidateProperty(const wchar_t* name, amf::AMFVariantStruct value, amf::AMFVariantStruct* pOutValidated) const;

	// Component
	MSABI virtual AMF_RESULT Init(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height);
	MSABI virtual AMF_RESULT ReInit(amf_int32 width,amf_int32 height);
	MSABI virtual AMF_RESULT Terminate();
	MSABI virtual AMF_RESULT Drain();
	MSABI virtual AMF_RESULT Flush();

	MSABI virtual AMF_RESULT SubmitInput(amf::AMFData* pData);
	MSABI virtual AMF_RESULT QueryOutput(amf::AMFData** ppData);
	MSABI virtual amf::AMFContext* GetContext();
	MSABI virtual AMF_RESULT SetOutputDataAllocatorCB(amf::AMFDataAllocatorCB* callback);

	MSABI virtual AMF_RESULT GetCaps(amf::AMFCaps** ppCaps);
	MSABI virtual AMF_RESULT Optimize(amf::AMFComponentOptimizationCallback* pCallback);
};

class AMFTraceImpl {

public:
	std::shared_ptr<amf::AMFTrace> inner;
	AMFTraceImpl(std::shared_ptr<amf::AMFTrace> inner) : inner(inner) {};
	MSABI virtual void TraceW(const wchar_t* src_path, amf_int32 line, amf_int32 level, const wchar_t* scope,amf_int32 countArgs, const wchar_t* format, ...);
	MSABI virtual void Trace(const wchar_t* src_path, amf_int32 line, amf_int32 level, const wchar_t* scope, const wchar_t* message, va_list* pArglist);

	MSABI virtual amf_int32 SetGlobalLevel(amf_int32 level);
	MSABI virtual amf_int32 GetGlobalLevel();

	MSABI virtual amf_bool EnableWriter(const wchar_t* writerID, bool enable);
	MSABI virtual amf_bool WriterEnabled(const wchar_t* writerID);
	MSABI virtual AMF_RESULT TraceEnableAsync(amf_bool enable);
	MSABI virtual AMF_RESULT TraceFlush();
	MSABI virtual AMF_RESULT SetPath(const wchar_t* path);
	MSABI virtual AMF_RESULT GetPath(wchar_t* path, amf_size* pSize);
	MSABI virtual amf_int32 SetWriterLevel(const wchar_t* writerID, amf_int32 level);
	MSABI virtual amf_int32 GetWriterLevel(const wchar_t* writerID);
	MSABI virtual amf_int32 SetWriterLevelForScope(const wchar_t* writerID, const wchar_t* scope, amf_int32 level);
	MSABI virtual amf_int32 GetWriterLevelForScope(const wchar_t* writerID, const wchar_t* scope);

	MSABI virtual amf_int32 GetIndentation();
	MSABI virtual void Indent(amf_int32 addIndent);

	MSABI virtual void RegisterWriter(const wchar_t* writerID, amf::AMFTraceWriter* pWriter, amf_bool enable);
	MSABI virtual void UnregisterWriter(const wchar_t* writerID);

	MSABI virtual const wchar_t* GetResultText(AMF_RESULT res);
	MSABI virtual const wchar_t* SurfaceGetFormatName(const amf::AMF_SURFACE_FORMAT eSurfaceFormat);
	MSABI virtual amf::AMF_SURFACE_FORMAT SurfaceGetFormatByName(const wchar_t* name);

	MSABI virtual const wchar_t* GetMemoryTypeName(const amf::AMF_MEMORY_TYPE memoryType);
	MSABI virtual amf::AMF_MEMORY_TYPE GetMemoryTypeByName(const wchar_t* name);

	MSABI virtual const wchar_t* GetSampleFormatName(const amf::AMF_AUDIO_FORMAT eFormat);
	MSABI virtual amf::AMF_AUDIO_FORMAT GetSampleFormatByName(const wchar_t* name);
};

class AMFFactoryImpl {
	std::unique_ptr<amf::AMFFactory> inner;

	std::unique_ptr<AMFTraceImpl> trace;
public:
	AMFFactoryImpl(amf::AMFFactory* inner) : inner(inner) {};

	virtual AMF_RESULT MSABI CreateContext(amf::AMFContext** ppContext);
	virtual AMF_RESULT MSABI CreateComponent(amf::AMFContext* pContext, const char16_t* id, amf::AMFComponent** ppComponent);
	virtual AMF_RESULT MSABI SetCacheFolder(const wchar_t* path);
	virtual const wchar_t* MSABI GetCacheFolder();
	virtual AMF_RESULT MSABI GetDebug(amf::AMFDebug** ppDebug);
	virtual AMF_RESULT MSABI GetTrace(amf::AMFTrace** ppTrace);
	virtual AMF_RESULT MSABI GetPrograms(amf::AMFPrograms** ppPrograms);
};

class AMFBufferImpl : AMFDataImpl
{
	std::shared_ptr<amf::AMFBuffer> inner;
public:
	AMFBufferImpl(std::shared_ptr<amf::AMFBuffer> inner): inner(inner), AMFDataImpl(inner) {};
	AMF_DECLARE_IID(0xb04b7248, 0xb6f0, 0x4321, 0xb6, 0x91, 0xba, 0xa4, 0x74, 0xf, 0x9f, 0xcb)

	MSABI virtual AMF_RESULT SetSize(amf_size newSize);
	MSABI virtual amf_size GetSize();
	MSABI virtual void* GetNative();
	MSABI virtual void AddObserver(amf::AMFBufferObserver* pObserver);
	MSABI virtual void RemoveObserver(amf::AMFBufferObserver* pObserver);
};

MSABI amf_long AMFInterfaceImpl::Acquire() {
	WINE_TRACE("()\n");

	auto ret = inner->Acquire();

	WINE_TRACE("=> %ld\n", ret);
	return ret;
}
MSABI amf_long AMFInterfaceImpl::Release() {
	WINE_TRACE("()\n");

	auto ret = inner->Release();

	WINE_TRACE("=> %ld\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFInterfaceImpl::QueryInterface(const amf::AMFGuid& interfaceID, void** ppInterface) {
	ZoneScoped;
	WINE_TRACE("(0x%08x:0x%04x:0x%04x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x:0x%02x, %p)\n",
		interfaceID.data1, interfaceID.data2, interfaceID.data3,
		interfaceID.data41, interfaceID.data42, interfaceID.data43, interfaceID.data44,
		interfaceID.data45, interfaceID.data46, interfaceID.data47, interfaceID.data48,
		ppInterface
	);

	if(interfaceID == amf::AMFBuffer::IID()) {
		WINE_TRACE("Retuning wrapped buffer\n");
		AMF_RESULT ret = inner->QueryInterface(interfaceID, ppInterface);
		if(*ppInterface != nullptr) *ppInterface = new AMFBufferImpl(std::shared_ptr<amf::AMFBuffer>((amf::AMFBuffer*)*ppInterface));

		return ret;
	}

	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFPropertyStorageImpl::SetProperty(const char16_t* name, amf::AMFVariantStruct value) {
	ZoneScoped;
	char32_t *linuxName = winWStrToLinux(name);
	WINE_TRACE("(%ls, %d)\n", (wchar_t*)linuxName, value.type);

	AMF_RESULT ret = inner->SetProperty((wchar_t*)linuxName, value);
	/* free(linuxName); */
	
	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFPropertyStorageImpl::GetProperty(const char16_t* name, amf::AMFVariantStruct* pValue) const {
	ZoneScoped;
	char32_t *linuxName = winWStrToLinux(name);
	WINE_TRACE("(%ls %p)\n", (wchar_t*)linuxName, pValue);

	AMF_RESULT ret = inner->GetProperty((wchar_t*)linuxName, pValue);
	WINE_TRACE("Value type is %d \n", pValue->type);

	/* free(linuxName); */
	WINE_TRACE("=> %d\n", ret);
	return ret;
}

MSABI amf_bool AMFPropertyStorageImpl::HasProperty(const char16_t* name) const {
	STUB();
	return false;
}
MSABI amf_size AMFPropertyStorageImpl::GetPropertyCount() const {
	STUB();
	return 0;
}
MSABI AMF_RESULT AMFPropertyStorageImpl::GetPropertyAt(amf_size index, char16_t* name, amf_size nameSize, amf::AMFVariantStruct* pValue) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFPropertyStorageImpl::Clear() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFPropertyStorageImpl::AddTo(amf::AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFPropertyStorageImpl::CopyTo(amf::AMFPropertyStorage* pDest, amf_bool deep) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI void AMFPropertyStorageImpl::AddObserver(amf::AMFPropertyStorageObserver* pObserver) {
	STUB();
}
MSABI void AMFPropertyStorageImpl::RemoveObserver(amf::AMFPropertyStorageObserver* pObserver) {
	STUB();
}

MSABI amf::AMF_MEMORY_TYPE AMFDataImpl::GetMemoryType() {
	STUB();
	return amf::AMF_MEMORY_TYPE::AMF_MEMORY_UNKNOWN;
}
MSABI AMF_RESULT AMFDataImpl::Duplicate(amf::AMF_MEMORY_TYPE type, AMFDataImpl** ppData) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFDataImpl::Convert(amf::AMF_MEMORY_TYPE type) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFDataImpl::Interop(amf::AMF_MEMORY_TYPE type) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf::AMF_DATA_TYPE AMFDataImpl::GetDataType() {
	STUB();
	return amf::AMF_DATA_TYPE::AMF_DATA_USER;
}
MSABI amf_bool AMFDataImpl::IsReusable() {
	STUB();
	return false;
}
MSABI void AMFDataImpl::SetPts(amf_pts pts) {
	STUB();
}
MSABI amf_pts AMFDataImpl::GetPts() {
	STUB();
	return 0;
}
MSABI void AMFDataImpl::SetDuration(amf_pts duration) {
	STUB();
}
MSABI amf_pts AMFDataImpl::GetDuration() {
	STUB();
	return 0;
}

MSABI amf::AMF_SURFACE_FORMAT AMFSurfaceImpl::GetFormat() {
	STUB();
	return amf::AMF_SURFACE_FORMAT::AMF_SURFACE_UNKNOWN;
}
MSABI amf_size AMFSurfaceImpl::GetPlanesCount() {
	STUB();
	return 0;
}
MSABI amf::AMFPlane* AMFSurfaceImpl::GetPlaneAt(amf_size index) {
	STUB();
	return nullptr;
}
MSABI amf::AMFPlane* AMFSurfaceImpl::GetPlane(amf::AMF_PLANE_TYPE type) {
	STUB();
	return nullptr;
}
MSABI amf::AMF_FRAME_TYPE AMFSurfaceImpl::GetFrameType() {
	STUB();
	return amf::AMF_FRAME_TYPE::AMF_FRAME_UNKNOWN;
}
MSABI void AMFSurfaceImpl::SetFrameType(amf::AMF_FRAME_TYPE type) {
	STUB();
}
MSABI AMF_RESULT AMFSurfaceImpl::SetCrop(amf_int32 x,amf_int32 y, amf_int32 width, amf_int32 height) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFSurfaceImpl::CopySurfaceRegion(amf::AMFSurface* pDest, amf_int32 dstX, amf_int32 dstY, amf_int32 srcX, amf_int32 srcY, amf_int32 width, amf_int32 height) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void AMFSurfaceImpl::AddObserver(amf::AMFSurfaceObserver* pObserver) {
	STUB();
}
MSABI void AMFSurfaceImpl::RemoveObserver(amf::AMFSurfaceObserver* pObserver) {
	STUB();
}

MSABI AMF_RESULT AMFBufferImpl::SetSize(amf_size newSize) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_size AMFBufferImpl::GetSize() {
	ZoneScoped;
	WINE_TRACE("()\n");
	amf_size ret = inner->GetSize();
	WINE_TRACE("=> %ld\n", ret);
	return ret;
}
MSABI void* AMFBufferImpl::GetNative() {
	ZoneScoped;
	WINE_TRACE("()\n");
	void* ret = inner->GetNative();
	WINE_TRACE("=> %p\n", ret);
	return ret;
}
MSABI void AMFBufferImpl::AddObserver(amf::AMFBufferObserver* pObserver) {
	STUB();
}
MSABI void AMFBufferImpl::RemoveObserver(amf::AMFBufferObserver* pObserver) {
	STUB();
}

MSABI AMF_RESULT AMFContextImpl::Terminate() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// DX9
MSABI AMF_RESULT AMFContextImpl::InitDX9(void* pDX9Device) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void* AMFContextImpl::GetDX9Device(amf::AMF_DX_VERSION dxVersionRequired) {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockDX9() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockDX9() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// DX11
MSABI AMF_RESULT AMFContextImpl::InitDX11(void* pDX11Device, amf::AMF_DX_VERSION dxVersionRequired) {
	ZoneScoped;
	WINE_TRACE("(%p, %d)\n", pDX11Device, dxVersionRequired);
	ID3D11Device *theirDevice = (ID3D11Device*)pDX11Device;

	IDXGIVkInteropDevice *dxvkDevice;
	if(theirDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), (void**)&dxvkDevice) != 0) {
		WINE_ERR("Given ID3D11Device doesn't support IDXGIVkInteropDevice. Only DXVK is supported.\n");
		return AMF_RESULT::AMF_FAIL;
	}

	VkInstance vkInstance;
	VkPhysicalDevice vkPhysDevice;
	VkDevice vkDevice;

	dxvkDevice->GetVulkanHandles(&vkInstance, &vkPhysDevice, &vkDevice);

	WINE_TRACE("Vulkan handles %p %p %p\n", vkInstance, vkPhysDevice, vkDevice);

	vkInstance = (VkInstance)wine_unwrap_instance(vkInstance);
	vkPhysDevice = (VkPhysicalDevice)wine_unwrap_phys_dev(vkPhysDevice);
	vkDevice = (VkDevice)wine_unwrap_device(vkDevice);

	WINE_TRACE("Vulkan handles after unwrap %p %p %p\n", vkInstance, vkPhysDevice, vkDevice);

	assert(device == nullptr);
	device = std::make_unique<amf::AMFVulkanDevice>(amf::AMFVulkanDevice{
		.cbSizeof = sizeof(amf::AMFVulkanDevice),
		.hInstance = vkInstance,
		.hPhysicalDevice = vkPhysDevice,
		.hDevice = vkDevice,
	});

	AMF_RESULT ret = inner->InitVulkan(device.get());

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI void* AMFContextImpl::GetDX11Device(amf::AMF_DX_VERSION dxVersionRequired) {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockDX11() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockDX11() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// OpenCL
MSABI AMF_RESULT AMFContextImpl::InitOpenCL(void* pCommandQueue) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void* AMFContextImpl::GetOpenCLContext() {
	STUB();
	return nullptr;
}
MSABI void* AMFContextImpl::GetOpenCLCommandQueue() {
	STUB();
	return nullptr;
}
MSABI void* AMFContextImpl::GetOpenCLDeviceID() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::GetOpenCLComputeFactory(amf::AMFComputeFactory **ppFactory) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::InitOpenCLEx(amf::AMFComputeDevice *pDevice) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::LockOpenCL() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockOpenCL() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// OpenGL
MSABI AMF_RESULT AMFContextImpl::InitOpenGL(amf_handle hOpenGLContext, amf_handle hWindow, amf_handle hDC) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_handle AMFContextImpl::GetOpenGLContext() {
	WINE_FIXME("Returning unwrapped OpenGL Context");
	amf_handle ret = inner->GetOpenGLContext();
	WINE_TRACE("() => %p \n", ret);
	return ret;
}
MSABI amf_handle AMFContextImpl::GetOpenGLDrawable() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockOpenGL() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockOpenGL() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// XV - Linux
MSABI AMF_RESULT AMFContextImpl::InitXV(void* pXVDevice) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void* AMFContextImpl::GetXVDevice() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockXV() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockXV() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// Gralloc - Android
MSABI AMF_RESULT AMFContextImpl::InitGralloc(void* pGrallocDevice) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void* AMFContextImpl::GetGrallocDevice() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockGralloc() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockGralloc() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// Allocation
MSABI AMF_RESULT AMFContextImpl::AllocBuffer(amf::AMF_MEMORY_TYPE type, amf_size size, amf::AMFBuffer** ppBuffer) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::AllocSurface(amf::AMF_MEMORY_TYPE type, amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf::AMFSurface** ppSurface) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::AllocAudioBuffer(amf::AMF_MEMORY_TYPE type, amf::AMF_AUDIO_FORMAT format, amf_int32 samples, amf_int32 sampleRate, amf_int32 channels, amf::AMFAudioBuffer** ppAudioBuffer) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// Wrap existing objects
MSABI AMF_RESULT AMFContextImpl::CreateBufferFromHostNative(void* pHostBuffer, amf_size size, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromHostNative(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf_int32 hPitch, amf_int32 vPitch, void* pData, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromDX9Native(void* pDX9Surface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromDX11Native(void* pDX11Surface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	ZoneScoped;
	WINE_TRACE("(%p %p %p)\n", pDX11Surface, ppSurface, pObserver);

	if(pDX11Surface == nullptr) {
		WINE_ERR("DX11 Surface cannot be NULL\n");
		return AMF_RESULT::AMF_INVALID_ARG;
	}

	ID3D11Texture2D *dx11Texture = (ID3D11Texture2D*)pDX11Surface;
	IDXGIVkInteropSurface *dxvkSurface;
	if(dx11Texture->QueryInterface(__uuidof(IDXGIVkInteropSurface), (void**)&dxvkSurface) != 0) {
		WINE_ERR("Given ID3DTexture2D doesn't support IDXVkInteropSurface. Only DXVK is supported.\n");
		return AMF_RESULT::AMF_FAIL;
	}

	VkImageCreateInfo createInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
	};
	VkImageLayout layout;
	VkImage image;
	if(dxvkSurface->GetVulkanImageInfo(&image, &layout, &createInfo) != 0) {
		WINE_TRACE("Couldn't get the vulkan data from the texture %p\n", dxvkSurface);
		return AMF_RESULT::AMF_FAIL;
	}
	WINE_TRACE("Vulkan texture %dx%d on device %p\n", createInfo.extent.width, createInfo.extent.height, ((amf::AMFVulkanDevice*)inner->GetVulkanDevice())->hDevice);

	VkDeviceMemory memory;
	VkDeviceSize memSize;
	if(dxvkSurface->GetVulkanDeviceMemory(&memory, &memSize) != 0) {
		WINE_TRACE("Couldn't get the vulkan device memory from the texture %p\n", dxvkSurface);
		return AMF_RESULT::AMF_FAIL;
	}
	memory = (VkDeviceMemory)wine_unwrap_dev_mem(memory);
	WINE_TRACE("Texture storage %p size %ld format %d\n", memory, memSize, createInfo.format);

	auto amfSurface = std::make_unique<amf::AMFVulkanSurface>(amf::AMFVulkanSurface {
		.cbSizeof = sizeof(amf::AMFVulkanSurface),
		.hImage = image,
		.hMemory = memory,
		.iSize = (int32_t)memSize,
		.eFormat = createInfo.format,
		.iWidth = (int32_t)createInfo.extent.width,
		.iHeight = (int32_t)createInfo.extent.height,
		.eCurrentLayout = VK_IMAGE_LAYOUT_GENERAL,
		.eUsage = createInfo.usage,
		.eAccess = amf::AMF_MEMORY_CPU_ACCESS_BITS::AMF_MEMORY_CPU_DEFAULT,
		.Sync = {
			.cbSizeof = sizeof(amf::AMFVulkanSync),
			.hSemaphore = nullptr,
			.bSubmitted = false,
			.hFence = nullptr,
		}
	});

	AMF_RESULT ret = inner->CreateSurfaceFromVulkanNative(amfSurface.get(), ppSurface, pObserver);
	if(*ppSurface != nullptr) {
		WINE_TRACE("Wrap surface %p\n", *ppSurface);
		*ppSurface = (amf::AMFSurface*)new AMFSurfaceImpl(std::shared_ptr<amf::AMFSurface>(*ppSurface), std::move(amfSurface), std::unique_ptr<ID3D11Texture2D>(dx11Texture));
	}

	WINE_TRACE("=> %d %p\n", ret, *ppSurface);
	return ret;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromOpenGLNative(amf::AMF_SURFACE_FORMAT format, amf_handle hGLTextureID, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromGrallocNative(amf_handle hGrallocSurface, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromOpenCLNative(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, void** pClPlanes, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateBufferFromOpenCLNative(void* pCLBuffer, amf_size size, amf::AMFBuffer** ppBuffer) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// Access to AMFCompute interface - AMF_MEMORY_OPENCL, AMF_MEMORY_COMPUTE_FOR_DX9, AMF_MEMORY_COMPUTE_FOR_DX11 are currently supported
MSABI AMF_RESULT AMFContextImpl::GetCompute(amf::AMF_MEMORY_TYPE eMemType, amf::AMFCompute** ppCompute) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFContextImpl::CreateBufferFromDX11Native(void* pHostBuffer, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFContextImpl::AllocBufferEx(amf::AMF_MEMORY_TYPE type, amf_size size, amf::AMF_BUFFER_USAGE usage, amf::AMF_MEMORY_CPU_ACCESS access, amf::AMFBuffer** ppBuffer) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::AllocSurfaceEx(amf::AMF_MEMORY_TYPE type, amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf::AMF_SURFACE_USAGE usage, amf::AMF_MEMORY_CPU_ACCESS access, amf::AMFSurface** ppSurface) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

// Vulkan - Windows, Linux
MSABI AMF_RESULT AMFContextImpl::InitVulkan(void* pVulkanDevice) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void* AMFContextImpl::GetVulkanDevice() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFContextImpl::LockVulkan() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::UnlockVulkan() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFContextImpl::CreateSurfaceFromVulkanNative(void* pVulkanImage, amf::AMFSurface** ppSurface, amf::AMFSurfaceObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::CreateBufferFromVulkanNative(void* pVulkanBuffer, amf::AMFBuffer** ppBuffer, amf::AMFBufferObserver* pObserver) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFContextImpl::GetVulkanDeviceExtensions(amf_size *pCount, const char **ppExtensions) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI void AMFIOCapsImpl::GetWidthRange(amf_int32* minWidth, amf_int32* maxWidth) const {
	WINE_TRACE("(%p %p)\n", minWidth, maxWidth);
	inner->GetWidthRange(minWidth, maxWidth);
}
MSABI void AMFIOCapsImpl::GetHeightRange(amf_int32* minHeight, amf_int32* maxHeight) const {
	WINE_TRACE("(%p %p)\n", minHeight, maxHeight);
	inner->GetHeightRange(minHeight, maxHeight);
}
MSABI amf_int32 AMFIOCapsImpl::GetVertAlign() const {
	STUB();
	return 0;
}
MSABI amf_int32 AMFIOCapsImpl::GetNumOfFormats() const {
	STUB();
	return 0;
}
MSABI AMF_RESULT AMFIOCapsImpl::GetFormatAt(amf_int32 index, amf::AMF_SURFACE_FORMAT* format, amf_bool* native) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFIOCapsImpl::GetNumOfMemoryTypes() const {
	STUB();
	return 0;
}
MSABI AMF_RESULT AMFIOCapsImpl::GetMemoryTypeAt(amf_int32 index, amf::AMF_MEMORY_TYPE* memType, amf_bool* native) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_bool AMFIOCapsImpl::IsInterlacedSupported() const {
	STUB();
	return false;
}

MSABI amf::AMF_ACCELERATION_TYPE AMFCapsImpl::GetAccelerationType() const {
	STUB();
	return amf::AMF_ACCELERATION_TYPE::AMF_ACCEL_NOT_SUPPORTED;
}
MSABI AMF_RESULT AMFCapsImpl::GetInputCaps(amf::AMFIOCaps** input) {
	WINE_TRACE("(%p)\n", input);

	amf::AMFIOCaps *realCaps;
	AMF_RESULT ret = inner->GetInputCaps(&realCaps);

	if(ret == AMF_RESULT::AMF_OK) {
		*input = (amf::AMFIOCaps*)new AMFIOCapsImpl(std::shared_ptr<amf::AMFIOCaps>(realCaps));
	}

	WINE_TRACE("=> %d)\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFCapsImpl::GetOutputCaps(amf::AMFIOCaps** output) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI amf_size AMFComponentImpl::GetPropertiesInfoCount() const {
	STUB();
	return 0;
}
MSABI AMF_RESULT AMFComponentImpl::GetPropertyInfo(amf_size index, const amf::AMFPropertyInfo** ppInfo) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFComponentImpl::GetPropertyInfo(const wchar_t* name, const amf::AMFPropertyInfo** ppInfo) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFComponentImpl::ValidateProperty(const wchar_t* name, amf::AMFVariantStruct value, amf::AMFVariantStruct* pOutValidated) const {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFComponentImpl::Init(amf::AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height) {
	WINE_TRACE("(%d %d %d)\n", format, width, height);

	AMF_RESULT ret = inner->Init(format, width, height);

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFComponentImpl::ReInit(amf_int32 width,amf_int32 height) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFComponentImpl::Terminate() {
	WINE_TRACE("()\n");

	AMF_RESULT ret = inner->Terminate();

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFComponentImpl::Drain() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFComponentImpl::Flush() {
	return inner->Flush();
}

uint32_t imgid = 0;
MSABI AMF_RESULT AMFComponentImpl::SubmitInput(amf::AMFData* pData) {
	ZoneScoped;
	WINE_TRACE("(%p)\n", pData);

	auto dataImpl = (AMFDataImpl*)pData;
	trace->TraceW((wchar_t*)U"main", 3232, AMF_TRACE_INFO, (wchar_t*)U"AS", 0, (wchar_t*)U"Submit some input!");
	WINE_TRACE("Got buffer of type %d\n", dataImpl->inner->GetMemoryType());

	AMFSurfaceImpl *data = (AMFSurfaceImpl*)dataImpl;

	IDXGIVkInteropSurface *dxvkSurface;
	IDXGIVkInteropDevice *vkDev = nullptr;
	if(data->d3dTexture->QueryInterface(__uuidof(IDXGIVkInteropSurface), (void**)&dxvkSurface) == 0) {
		WINE_TRACE("YES. Surface is from dxvk!\n");
		dxvkSurface->GetDevice(&vkDev);

		vkDev->FlushRenderingCommands();
		vkDev->LockSubmissionQueue();

		VkInstance instance = 0;
		VkPhysicalDevice physicalDevice = 0;
		VkDevice device = 0;
		vkDev->GetVulkanHandles(&instance, &physicalDevice, &device);

		VkQueue queue;
		uint32_t queueFamily;
		vkDev->GetSubmissionQueue(&queue, &queueFamily);

		VkImage image;
		VkImageLayout layout;
		VkImageCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.pNext = nullptr,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
		};
		dxvkSurface->GetVulkanImageInfo(&image, &layout, &createInfo);
		
		VkDeviceMemory memory;
		VkDeviceSize size;
		dxvkSurface->GetVulkanDeviceMemory(&memory, &size);

		char filename[32];
		sprintf(filename, "img/%d.dat", imgid++);
		FILE *f;
		/* FILE *f = fopen(filename, "w"); */

		instance = (VkInstance)wine_unwrap_instance(instance);
		physicalDevice = (VkPhysicalDevice)wine_unwrap_phys_dev(physicalDevice);
		device = (VkDevice)wine_unwrap_device(device);
		queue = (VkQueue)wine_unwrap_queue(queue);
		memory = (VkDeviceMemory)wine_unwrap_dev_mem(memory);
		WINE_TRACE("Vulkan handles after unwrap %p %p %p\n", device, queue, physicalDevice);
		WINE_TRACE("Image extent %ux%u layout %d tiling %d\n", createInfo.extent.width, createInfo.extent.height, createInfo.format, createInfo.tiling);
		/* saveScreenshot(f, instance, device, queue, queueFamily, physicalDevice, createInfo.format, image, createInfo.extent.width, createInfo.extent.height, createInfo.tiling, memory, size); */

		// We are clearly missing synchronization somewhere. But where?
		// This at least gives me video output.
		/* usleep(20000); */

		/* fclose(f); */
	}

	/* WINE_TRACE("Real amf surface %p\n", data->inner.get()); */
	auto ret = inner->SubmitInput(dataImpl->inner.get());

	if(vkDev != nullptr) vkDev->ReleaseSubmissionQueue();

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFComponentImpl::QueryOutput(amf::AMFData** ppData) {
	ZoneScoped;
	WINE_TRACE("(%p)\n", ppData);

	AMF_RESULT ret = inner->QueryOutput(ppData);
	if(*ppData == nullptr) goto done;

	WINE_TRACE("Got buffer of type %d\n", (*ppData)->GetMemoryType());

	if((*ppData)->GetMemoryType() == 1) {
		amf::AMFBuffer *buf = (amf::AMFBuffer*)*ppData;
		fwrite(buf->GetNative(), 1, buf->GetSize(), videofile);
		fflush(videofile);
	}

	*ppData = (amf::AMFData*)new AMFDataImpl(std::shared_ptr<amf::AMFData>(*ppData));

done:
	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI amf::AMFContext* AMFComponentImpl::GetContext() {
	STUB();
	return nullptr;
}
MSABI AMF_RESULT AMFComponentImpl::SetOutputDataAllocatorCB(amf::AMFDataAllocatorCB* callback) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI AMF_RESULT AMFComponentImpl::GetCaps(amf::AMFCaps** ppCaps) {
	WINE_TRACE("(%p)\n", ppCaps);

	amf::AMFCaps *realCaps;
	AMF_RESULT ret = inner->GetCaps(&realCaps);

	if(ret == AMF_RESULT::AMF_OK) {
		*ppCaps = (amf::AMFCaps*)new AMFCapsImpl(std::shared_ptr<amf::AMFCaps>(realCaps));
	}

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
MSABI AMF_RESULT AMFComponentImpl::Optimize(amf::AMFComponentOptimizationCallback* pCallback) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

MSABI void AMFTraceImpl::TraceW(const wchar_t* src_path, amf_int32 line, amf_int32 level, const wchar_t* scope,amf_int32 countArgs, const wchar_t* format, ...) {
	STUB();
}
MSABI void AMFTraceImpl::Trace(const wchar_t* src_path, amf_int32 line, amf_int32 level, const wchar_t* scope, const wchar_t* message, va_list* pArglist) {
	STUB();
}
MSABI amf_int32 AMFTraceImpl::SetGlobalLevel(amf_int32 level) {
	STUB();
	return 0;
}
MSABI amf_int32 AMFTraceImpl::GetGlobalLevel() {
	STUB();
	return 0;
}
MSABI amf_bool AMFTraceImpl::EnableWriter(const wchar_t* writerID, bool enable) {
	STUB();
	return false;
}
MSABI amf_bool AMFTraceImpl::WriterEnabled(const wchar_t* writerID) {
	STUB();
	return false;
}
MSABI AMF_RESULT AMFTraceImpl::TraceEnableAsync(amf_bool enable) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFTraceImpl::TraceFlush() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFTraceImpl::SetPath(const wchar_t* path) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI AMF_RESULT AMFTraceImpl::GetPath(wchar_t* path, amf_size* pSize) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFTraceImpl::SetWriterLevel(const wchar_t* writerID, amf_int32 level) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFTraceImpl::GetWriterLevel(const wchar_t* writerID) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFTraceImpl::SetWriterLevelForScope(const wchar_t* writerID, const wchar_t* scope, amf_int32 level) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFTraceImpl::GetWriterLevelForScope(const wchar_t* writerID, const wchar_t* scope) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI amf_int32 AMFTraceImpl::GetIndentation() {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}
MSABI void AMFTraceImpl::Indent(amf_int32 addIndent) {
	STUB();
}
MSABI void AMFTraceImpl::RegisterWriter(const wchar_t* writerID, amf::AMFTraceWriter* pWriter, amf_bool enable) {
	STUB();
}
MSABI void AMFTraceImpl::UnregisterWriter(const wchar_t* writerID) {
	STUB();
}
MSABI const wchar_t* AMFTraceImpl::GetResultText(AMF_RESULT res) {
	WINE_TRACE("(%d)\n", res);
	auto ret = inner->GetResultText(res);
	WINE_TRACE("=> %ls\n", ret);
	return (wchar_t*)linuxWStrToWin((char32_t*)ret);
}
MSABI const wchar_t* AMFTraceImpl::SurfaceGetFormatName(const amf::AMF_SURFACE_FORMAT eSurfaceFormat) {
	STUB();
	return nullptr;
}
MSABI amf::AMF_SURFACE_FORMAT AMFTraceImpl::SurfaceGetFormatByName(const wchar_t* name) {
	STUB();
	return amf::AMF_SURFACE_FORMAT::AMF_SURFACE_UNKNOWN;
}
MSABI const wchar_t* AMFTraceImpl::GetMemoryTypeName(const amf::AMF_MEMORY_TYPE memoryType) {
	STUB();
	return nullptr;
}
MSABI amf::AMF_MEMORY_TYPE AMFTraceImpl::GetMemoryTypeByName(const wchar_t* name) {
	STUB();
	return amf::AMF_MEMORY_TYPE::AMF_MEMORY_UNKNOWN;
}
MSABI const wchar_t* AMFTraceImpl::GetSampleFormatName(const amf::AMF_AUDIO_FORMAT eFormat) {
	STUB();
	return nullptr;
}
MSABI amf::AMF_AUDIO_FORMAT AMFTraceImpl::GetSampleFormatByName(const wchar_t* name) {
	STUB();
	return amf::AMF_AUDIO_FORMAT::AMFAF_UNKNOWN;
}

AMF_RESULT MSABI AMFFactoryImpl::CreateContext(amf::AMFContext** ppContext) {

	amf::AMFContext *realContext;
	AMF_RESULT ret = inner->CreateContext(&realContext);
	amf::AMFContext1 * realContext1;
	assert(realContext->QueryInterface(amf::AMFContext1::IID(), (void**)&realContext1) == AMF_RESULT::AMF_OK);

	*ppContext = (amf::AMFContext*)new AMFContextImpl(std::shared_ptr<amf::AMFContext1>(realContext1));
	WINE_TRACE("(%p) => %d\n", ppContext, ret);
	return ret;
}
AMF_RESULT MSABI AMFFactoryImpl::CreateComponent(amf::AMFContext* pContext, const char16_t* id, amf::AMFComponent** ppComponent) {
	AMFContextImpl *ourContext = (AMFContextImpl*)pContext;
	char32_t *linuxId = winWStrToLinux(id);
	WINE_TRACE("(%p %ls %p)\n", pContext, linuxId, ppComponent);

	amf::AMFComponent *realComponent;
	AMF_RESULT ret = inner->CreateComponent(ourContext->inner.get(), (wchar_t*)linuxId, &realComponent);
	/* free(linuxId); */

	if(ret == AMF_RESULT::AMF_OK) {
		*ppComponent = (amf::AMFComponent*)new AMFComponentImpl(std::shared_ptr<amf::AMFComponent>(realComponent), this->trace->inner.get());
	}

	WINE_TRACE("=> %d\n", ret);
	return ret;
}
AMF_RESULT MSABI AMFFactoryImpl::SetCacheFolder(const wchar_t* path) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
	/* WINE_TRACE("%s\n", __PRETTY_FUNCTION__); */
	/* return inner->SetCacheFolder(path); */
}
const wchar_t* MSABI AMFFactoryImpl::GetCacheFolder() {
	WINE_TRACE("%s\n", __PRETTY_FUNCTION__);
	return inner->GetCacheFolder();
}
AMF_RESULT MSABI AMFFactoryImpl::GetDebug(amf::AMFDebug** ppDebug) {
	STUB();
	inner->GetDebug(ppDebug);
	(*ppDebug)->AssertsEnable(true);
	return AMF_RESULT::AMF_FAIL;
}
AMF_RESULT MSABI AMFFactoryImpl::GetTrace(amf::AMFTrace** ppTrace) {
	WINE_TRACE("%s\n", __PRETTY_FUNCTION__);

	amf::AMFTrace* realTrace;
	AMF_RESULT ret = inner->GetTrace(&realTrace);
	if(ret != AMF_RESULT::AMF_OK) goto end;

	realTrace->SetGlobalLevel(AMF_TRACE_NOLOG);
	realTrace->SetPath((wchar_t*)U"/home/delusional/Documents/vrdriver/vrdriver/amf_trace");
	realTrace->SetWriterLevel((wchar_t*)U"File", AMF_TRACE_INFO);
	realTrace->EnableWriter((wchar_t*)U"File", true);
	/* realTrace->TraceW((wchar_t*)U"main", 3232, AMF_TRACE_DEBUG, (wchar_t*)U"AS", 0, (wchar_t*)U"It's a message"); */

	this->trace = std::make_unique<AMFTraceImpl>(std::shared_ptr<amf::AMFTrace>(realTrace)); 

	*ppTrace = (amf::AMFTrace*)this->trace.get();
end:
	WINE_TRACE("=> %d\n", ret);
	return ret;
}
AMF_RESULT MSABI AMFFactoryImpl::GetPrograms(amf::AMFPrograms** ppPrograms) {
	STUB();
	return AMF_RESULT::AMF_FAIL;
}

void *lib = nullptr;
AMFFactoryImpl *factory;

DLL_EXPORT AMF_RESULT AMFInit(uint64_t version, amf::AMFFactory **out) {
	WINE_TRACE("(%ld)\n", version);

	void* vulkanHandle = dlopen("libvulkan.so", RTLD_LAZY | RTLD_GLOBAL);
	p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vulkanHandle, "vkGetInstanceProcAddr");

	if(lib == nullptr) {
		lib = dlopen(AMF_DLL_NAMEA, RTLD_NOW);
		if(lib == nullptr) {
			WINE_ERR("AMF failed to load %s\n", dlerror());
			assert(lib != nullptr);
		}
	}

	if(factory == nullptr) {
		AMFInit_Fn initfn = (AMFInit_Fn)dlsym(lib, AMF_INIT_FUNCTION_NAME);
		if(initfn == nullptr) {
			WINE_ERR("Couldn't find the AMFInit function: %s\n", dlerror());
			assert(initfn != nullptr);
		}

		amf::AMFFactory *realFactory;
		AMF_RESULT res = initfn(version, &realFactory);
		assert(res == AMF_RESULT::AMF_OK);
		factory = new AMFFactoryImpl(realFactory);
	}

	*out = (amf::AMFFactory*)factory;
	return AMF_RESULT::AMF_OK;
}

DLL_EXPORT AMF_RESULT AMFQueryVersion(uint64_t *version) {
	WINE_TRACE("%s\n", __PRETTY_FUNCTION__);
	if(lib == nullptr) {
		lib = dlopen(AMF_DLL_NAMEA, RTLD_NOW);
		if(lib == nullptr) {
			WINE_ERR("AMF failed to load %s\n", dlerror());
			assert(lib != nullptr);
		}
	}

	AMFQueryVersion_Fn fn = (AMFQueryVersion_Fn)dlsym(lib, AMF_QUERY_VERSION_FUNCTION_NAME);
	if(fn == nullptr) {
		WINE_ERR("Couldn't find the AMFQueryVersion function: %s\n", dlerror());
		assert(fn != nullptr);
	}

	return fn(version);
}
