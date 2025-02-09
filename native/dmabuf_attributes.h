#include <cstdint>

#pragma pack(push, 8)
static const uint32_t MaxDmabufPlaneCount = 4;
struct DmabufPlane_t
{
	uint32_t unOffset;
	uint32_t unStride;
	int32_t nFd;
};
struct DmabufAttributes_t
{
	void *pNext; // MUST be NULL. Unused right now, but could be used to extend this structure in the future.
	uint32_t unWidth;
	uint32_t unHeight;
	uint32_t unDepth;
	uint32_t unMipLevels;
	uint32_t unArrayLayers;
	uint32_t unSampleCount;
	uint32_t unFormat;   // DRM_FORMAT_
	uint64_t ulModifier; // DRM_FORMAT_MOD_
	uint32_t unPlaneCount;
	DmabufPlane_t plane[MaxDmabufPlaneCount];
};
#pragma pack(pop)
