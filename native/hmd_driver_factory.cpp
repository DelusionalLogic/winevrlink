#include "device_provider.h"
#include "ipc.h"

#include "openvr_driver.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <libdrm/drm_fourcc.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dmabuf_attributes.h"

using namespace vr;

#define STUB(pipe) \
do { \
	pipe.msg("Unimplemented stub %s\n", __PRETTY_FUNCTION__); \
}while(0)

struct DxvkSharedTextureMetadata {
	uint32_t             Width;
	uint32_t             Height;
	uint32_t             MipLevels;
	uint32_t             ArraySize;
	uint32_t             Format;
	struct {
		uint32_t Count;
		uint32_t Quality;
	} SampleDesc;
	uint32_t      Usage;
	uint32_t             BindFlags;
	uint32_t             CPUAccessFlags;
	uint32_t             MiscFlags;
	uint32_t             RowPitch;
	uint64_t         DRMFormat;
	uint32_t TextureLayout;
};


typedef uint64_t handle;

static void handler(enum PipeMethod, void*);

namespace vr {
typedef uint64_t PathHandle_t;

typedef struct PathWrite_t
{
	PathHandle_t ulPath;
	EPropertyWriteType writeType;
	ETrackedPropertyError eSetError;
	void * pvBuffer; // void *
	uint32_t unBufferSize;
	PropertyTypeTag_t unTag;
	ETrackedPropertyError eError;
	char * pszPath; // const char *
} PathWrite_t;

typedef struct PathRead_t
{
	PathHandle_t ulPath;
	void * pvBuffer; // void *
	uint32_t unBufferSize;
	PropertyTypeTag_t unTag;
	uint32_t unRequiredBufferSize;
	ETrackedPropertyError eError;
	char * pszPath; // const char *
} PathRead_t;

class IVRPaths
{
public:
	virtual ETrackedPropertyError ReadPathBatch(PropertyContainerHandle_t ulRootHandle, struct PathRead_t * pBatch, uint32_t unBatchEntryCount) = 0;
	virtual ETrackedPropertyError WritePathBatch(PropertyContainerHandle_t ulRootHandle, struct PathWrite_t * pBatch, uint32_t unBatchEntryCount) = 0;
	virtual ETrackedPropertyError StringToHandle(PathHandle_t * pHandle, char * pchPath) = 0;
	virtual ETrackedPropertyError HandleToString(PathHandle_t pHandle, char * pchBuffer, uint32_t unBufferSize, uint32_t * punBufferSizeUsed) = 0;
};
}

Pipe global_pipe;

//Global, single interface of our device provider.
DeviceProvider device_provider;

class VRDriverDirect : IVRDriverDirectModeComponent
{
	uint64_t objId;

	uint8_t refs = 0;

	// @PERF: This should be a real datastructure. Back when it was a couple of
	// textures, I could justify the argument that it was probably faster to
	// probe an array, but this is a lot of textures.
	uint32_t owner[3*80];
	uint64_t ourRefs[3*80];
	uint64_t theirRefs[3*80];

	bool TranslateToTheirs(vr::SharedTextureHandle_t ours, vr::SharedTextureHandle_t *theirs);
	void MapTextures(vr::SharedTextureHandle_t **ours, vr::SharedTextureHandle_t **theirs, uint32_t **pids);
	bool FindFromOurs(vr::SharedTextureHandle_t needle, size_t *i);
	bool FindFromOwner(uint32_t needle, size_t *index);
public:
	VRDriverDirect(uint64_t objId) : objId(objId) {};

	virtual void CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc, SwapTextureSet_t *pOutSwapTextureSet );
	virtual void DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle );
	virtual void DestroyAllSwapTextureSets( uint32_t unPid );
	virtual void GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[ 2 ], uint32_t( *pIndices )[ 2 ] );
	virtual void SubmitLayer( const SubmitLayerPerEye_t( &perEye )[ 2 ] );
	virtual void Present( vr::SharedTextureHandle_t syncTexture );
	virtual void PostPresent( const Throttling_t *pThrottling );
	virtual void GetFrameTiming( DriverDirectMode_FrameTiming *pFrameTiming );
};

class IVRIPCResourceManagerClient2 {
public:
	virtual bool NewSharedVulkanImage( uint32_t nImageFormat, uint32_t nWidth, uint32_t nHeight, bool bRenderable, bool bMappable, bool bComputeAccess, uint32_t unMipLevels, uint32_t unArrayLayerCount, vr::SharedTextureHandle_t *pSharedHandle ) = 0;
	virtual bool NewSharedVulkanBuffer( uint32_t nSize, uint32_t nUsageFlags, vr::SharedTextureHandle_t *pSharedHandle ) = 0;
	virtual bool NewSharedVulkanSemaphore( vr::SharedTextureHandle_t *pSharedHandle ) = 0;
	virtual bool RefResource( vr::SharedTextureHandle_t hSharedHandle, uint64_t *pNewIpcHandle ) = 0;
	virtual bool UnrefResource( vr::SharedTextureHandle_t hSharedHandle ) = 0;
	virtual bool GetDmabufFormats( uint32_t *pOutFormatCount, uint32_t *pOutFormats ) = 0;
	virtual bool GetDmabufModifiers( vr::EVRApplicationType eApplicationType, uint32_t unDRMFormat, uint32_t *pOutModifierCount, uint64_t *pOutModifiers ) = 0;
	virtual bool ImportDmabuf( vr::EVRApplicationType eApplicationType, DmabufAttributes_t *pDmabufAttributes, vr::SharedTextureHandle_t *pSharedHandle ) = 0;
	virtual bool ReceiveSharedFd( uint64_t ulIpcHandle, int *pOutFd ) = 0;
};

void VRDriverDirect::CreateSwapTextureSet( uint32_t unPid, const SwapTextureSetDesc_t *pSwapTextureSetDesc, SwapTextureSet_t *pOutSwapTextureSet ) {
	global_pipe.msg("call CreateSwapTextureSet(%d, %p, %p)\n", unPid, pSwapTextureSetDesc, pOutSwapTextureSet);
	global_pipe.msg("%d %d %d %d\n", pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount);
	// I think this is in VkFormat even though the documentation states it's in DXGI_FORMAT
	assert(pSwapTextureSetDesc->nFormat == 43);

	vr::SharedTextureHandle_t *ours;
	vr::SharedTextureHandle_t *theirs;
	uint32_t *pids;
	MapTextures(&ours, &theirs, &pids);

	global_pipe.begin_call(METH_DIRECT_CSWAP);
	global_pipe.send(&this->objId, sizeof(uint64_t));
	global_pipe.send(&unPid, sizeof(unPid));
	// VKFormat 43 translates to DXGI format 29
	SwapTextureSetDesc_t requestedTexture = {
		.nWidth = pSwapTextureSetDesc->nWidth,
		.nHeight = pSwapTextureSetDesc->nHeight,
		.nFormat = 29,
		.nSampleCount = pSwapTextureSetDesc->nSampleCount
	};
	global_pipe.send(&requestedTexture, sizeof(requestedTexture));


	global_pipe.wait_for_return();
	global_pipe.recv(&pOutSwapTextureSet->unTextureFlags, sizeof(pOutSwapTextureSet->unTextureFlags));
	int fds[3];
	global_pipe.recv_fd(&fds[0]);
	global_pipe.recv_fd(&fds[1]);
	global_pipe.recv_fd(&fds[2]);
	global_pipe.msg("Recv fds %d %d %d\n", fds[0], fds[1], fds[2]);
	global_pipe.recv(&theirs[0], sizeof(theirs[0]));
	global_pipe.recv(&theirs[1], sizeof(theirs[1]));
	global_pipe.recv(&theirs[2], sizeof(theirs[2]));
	uint32_t pitches[3];
	global_pipe.recv(&pitches[0], sizeof(pitches[0]));
	global_pipe.recv(&pitches[1], sizeof(pitches[1]));
	global_pipe.recv(&pitches[2], sizeof(pitches[2]));
	global_pipe.return_read_channel();

	IVRIPCResourceManagerClient2 *resMan = (IVRIPCResourceManagerClient2*)vr::VRIPCResourceManager();
	assert(resMan != NULL);
	for(uint8_t i = 0; i < 3; i++) {
		global_pipe.msg("Importing texture %d\n", i);
		struct DmabufAttributes_t dma = {
			.pNext = nullptr,
			.unWidth = pSwapTextureSetDesc->nWidth,
			.unHeight = pSwapTextureSetDesc->nHeight,
			.unDepth = 1,
			.unMipLevels = 1,
			.unArrayLayers = 1,
			.unSampleCount = 1,
			// This fits VkFormat 43 R8G8B8A8 I think
			.unFormat = DRM_FORMAT_ABGR8888,
			.ulModifier = DRM_FORMAT_MOD_LINEAR,
			.unPlaneCount = 1,
			.plane = {{
				.unOffset = 0,
				.unStride = pitches[i],
				.nFd = fds[i],
			}}
		};
		vr::SharedTextureHandle_t sharedHandle = 0;
		global_pipe.msg("Begin remote call %lu, %u\n", sharedHandle, unPid);
		uint64_t success = resMan->ImportDmabuf(vr::EVRApplicationType::VRApplication_Other, &dma, &sharedHandle);
		close(fds[i]);
		if(success != 1) {
			global_pipe.msg("Import Dmabuf failed %d\n", success);
			abort();
		}

		pids[i] = unPid;
		ours[i] = sharedHandle;
		pOutSwapTextureSet->rSharedTextureHandles[i] = sharedHandle;
		global_pipe.msg("Texture %d ref %p imported %p for %d\n", i, theirs[i], ours[i], pids[i]);
	}
	global_pipe.msg("We now hold %d textures\n", refs);
	global_pipe.msg("ret %d %p %p %p\n", pOutSwapTextureSet->unTextureFlags, pOutSwapTextureSet->rSharedTextureHandles[0], pOutSwapTextureSet->rSharedTextureHandles[1], pOutSwapTextureSet->rSharedTextureHandles[2]);
}
void VRDriverDirect::DestroySwapTextureSet( vr::SharedTextureHandle_t sharedTextureHandle ) {
	global_pipe.msg("call DestroySwapTextureSet(%p)\n", sharedTextureHandle);


	size_t i = 0;
	if(!FindFromOurs(sharedTextureHandle, &i)) {
		global_pipe.msg("Unknown our ref %p skip\n", sharedTextureHandle);
	}

	IVRIPCResourceManagerClient2 *resMan = (IVRIPCResourceManagerClient2*)vr::VRIPCResourceManager();
	resMan->UnrefResource(ourRefs[i]);

	if(i != refs-1) {
		// We need to copy something into this slot. We don't swap since we are
		// going to free the other slot anyway
		theirRefs[i] = theirRefs[refs-1];
		ourRefs[i] = ourRefs[refs-1];
		owner[i] = owner[refs-1];
	}

	refs--;
	global_pipe.msg("ret\n");
}
void VRDriverDirect::DestroyAllSwapTextureSets( uint32_t unPid ) {
	global_pipe.msg("call DestroyAllSwapTextureSets(%d)\n", unPid);

	IVRIPCResourceManagerClient2 *resMan = (IVRIPCResourceManagerClient2*)vr::VRIPCResourceManager();
	size_t dst = 0;
	for(size_t i = 0; i < refs; i++) {
		if(owner[i] == unPid) {
			resMan->UnrefResource(ourRefs[i]);
			continue;
		}

		owner[dst] = owner[i];
		ourRefs[dst] = ourRefs[i];
		theirRefs[dst] = theirRefs[i];
		dst++;
	}
	global_pipe.msg("Destroyed %d textures\n", refs - dst);

	refs = dst;

	global_pipe.msg("ret\n");
}
bool VRDriverDirect::TranslateToTheirs(vr::SharedTextureHandle_t ours, vr::SharedTextureHandle_t *theirs) {
	size_t i = 0;
	*theirs = 0;
	if(ours == 0) return true; // our 0 explicitly maps to their 0
	if(!FindFromOurs(ours, &i)) return false;
	*theirs = theirRefs[i];
	return true;
}
void VRDriverDirect::MapTextures(vr::SharedTextureHandle_t **ours, vr::SharedTextureHandle_t **theirs, uint32_t **pids) {
	// Make sure we have space for 3 textures
	assert(refs <= (sizeof(ourRefs)/sizeof(ourRefs[0])) - 3);

	*ours = ourRefs + refs;
	*theirs = theirRefs + refs;
	*pids = owner + refs;

	refs += 3;
}
bool VRDriverDirect::FindFromOurs(vr::SharedTextureHandle_t needle, size_t *index) {
	if(needle == 0) return false;

	for(; *index < refs; (*index)++) {
		if(ourRefs[*index] == needle) {
			return true;
		}
	}
	return false;
}
void VRDriverDirect::GetNextSwapTextureSetIndex( vr::SharedTextureHandle_t sharedTextureHandles[ 2 ], uint32_t( *pIndices )[ 2 ] ) {
#if 0
	STUB(global_pipe);
	return;
#else
	global_pipe.msg("call GetNextSwapTextureSetIndex(%p, %p, %p)\n", sharedTextureHandles[0], sharedTextureHandles[1], pIndices);

	vr::SharedTextureHandle_t theirRef[2];
	for(uint8_t i = 0; i < 2; i++) {
		if(!TranslateToTheirs(sharedTextureHandles[i], &theirRef[i])) {
			global_pipe.msg("Unknown our ref %p skip\n", sharedTextureHandles[i]);
			theirRefs[i] = 0;
			//return;
		}
	}

	global_pipe.begin_call(METH_DIRECT_NEXT);
	global_pipe.send(&this->objId, sizeof(objId));

	global_pipe.send(&theirRef[0], sizeof(theirRef[0]));
	global_pipe.send(&theirRef[1], sizeof(theirRef[1]));
	global_pipe.send(pIndices, sizeof(*pIndices));

	global_pipe.wait_for_return();

	global_pipe.recv(pIndices, sizeof(*pIndices));

	global_pipe.return_read_channel();

	global_pipe.msg("ret %d %d\n", (*pIndices)[0], (*pIndices)[1]);
#endif
}
void VRDriverDirect::SubmitLayer( const SubmitLayerPerEye_t( &perEye )[ 2 ] ) {
	global_pipe.msg("call SubmitLayer(%p, %p, %p, %p)\n", perEye[0].hTexture, perEye[0].hDepthTexture, perEye[1].hTexture, perEye[1].hDepthTexture);

	vr::SharedTextureHandle_t ourRef[4] = {0};
	for(uint8_t i = 0; i < 2; i++) {
		ourRef[i] = perEye[i].hTexture;
		ourRef[i+2] = perEye[i].hDepthTexture;
	}

	vr::SharedTextureHandle_t theirRef[4] = {0};
	for(uint8_t i = 0; i < 4; i++) {
		if(!TranslateToTheirs(ourRef[i], &theirRef[i])) {
			global_pipe.msg("Unknown our ref %p\n", ourRef[i]);
			return;
		}
	}

	global_pipe.begin_call(METH_DIRECT_SUBMIT);
	global_pipe.send(&this->objId, sizeof(objId));

	for(uint8_t i = 0; i < 2; i++) {
		global_pipe.send(&theirRef[i], sizeof(theirRef[i]));
		global_pipe.send(&theirRef[i+2], sizeof(theirRef[i+2]));

		global_pipe.send(&perEye[i].bounds, sizeof(perEye[i].bounds));
		global_pipe.send(&perEye[i].mProjection, sizeof(perEye[i].mProjection));
		global_pipe.send(&perEye[i].mHmdPose, sizeof(perEye[i].mHmdPose));
		global_pipe.send(&perEye[i].flHmdPosePredictionTimeInSecondsFromNow, sizeof(perEye[i].flHmdPosePredictionTimeInSecondsFromNow));
	}

	global_pipe.wait_for_return();
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
void VRDriverDirect::Present( vr::SharedTextureHandle_t syncTexture ) {
	global_pipe.msg("call Present(%p)\n", syncTexture);

	vr::SharedTextureHandle_t theirRef;
	if(!TranslateToTheirs(syncTexture, &theirRef)) {
		global_pipe.msg("Unknown our ref %p skip\n", syncTexture);
		return;
	}

	global_pipe.begin_call(METH_DIRECT_PRESENT);
	global_pipe.send(&this->objId, sizeof(objId));

	global_pipe.send(&theirRef, sizeof(theirRef));

	global_pipe.wait_for_return();
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
void VRDriverDirect::PostPresent( const Throttling_t *pThrottling ) {
	global_pipe.msg("call PostPresent(%p)\n", pThrottling);

	global_pipe.begin_call(METH_DIRECT_POSTPRES);
	global_pipe.send(&this->objId, sizeof(objId));

	global_pipe.send(pThrottling, sizeof(*pThrottling));

	global_pipe.wait_for_return();
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
void VRDriverDirect::GetFrameTiming( DriverDirectMode_FrameTiming *pFrameTiming ) {
	global_pipe.msg("call GetFrameTiming(%p)\n", pFrameTiming);

	global_pipe.begin_call(METH_DIRECT_FTIME);
	global_pipe.send(&this->objId, sizeof(objId));

	global_pipe.wait_for_return();

	global_pipe.recv(pFrameTiming, sizeof(*pFrameTiming));

	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}

class VRDisplayComponent : public vr::IVRDisplayComponent {
	uint64_t objId;
public:
	VRDisplayComponent(uint64_t objId) : objId(objId) {};

	virtual void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
	virtual bool IsDisplayOnDesktop( );
	virtual bool IsDisplayRealDisplay( );
	virtual void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight );
	virtual void GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
	virtual void GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom );
	virtual vr::DistortionCoordinates_t ComputeDistortion( vr::EVREye eEye, float fU, float fV );
	virtual bool ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV );
};

void VRDisplayComponent::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) {
	global_pipe.msg("call GetWindowBounds(%p, %p, %p, %p)\n", pnX, pnY, pnWidth, pnHeight);

	global_pipe.begin_call(METH_COMP_WINSIZE);
	global_pipe.send(&this->objId, sizeof(uint64_t));

	global_pipe.wait_for_return();

	global_pipe.recv(pnX, sizeof(*pnX));
	global_pipe.recv(pnY, sizeof(*pnY));
	global_pipe.recv(pnWidth, sizeof(*pnWidth));
	global_pipe.recv(pnHeight, sizeof(*pnHeight));
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
bool VRDisplayComponent::IsDisplayOnDesktop( ) {
	global_pipe.msg("call IsDisplayOnDesktop()\n");

	global_pipe.begin_call(METH_COMP_ONDESKTOP);
	global_pipe.send(&this->objId, sizeof(uint64_t));

	global_pipe.wait_for_return();

	bool ret;
	global_pipe.recv(&ret, sizeof(ret));
	global_pipe.return_read_channel();

	global_pipe.msg("ret %d\n", ret);
	return ret;
}
bool VRDisplayComponent::IsDisplayRealDisplay( ) {
	global_pipe.msg("call IsDisplayRealDisplay()\n");

	global_pipe.begin_call(METH_COMP_REALDISPLAY);
	global_pipe.send(&this->objId, sizeof(uint64_t));

	global_pipe.wait_for_return();

	bool ret;
	global_pipe.recv(&ret, sizeof(ret));
	global_pipe.return_read_channel();

	global_pipe.msg("ret %d\n", ret);
	return ret;
}
void VRDisplayComponent::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) {
	global_pipe.msg("call GetRecommendedRenderTargetSize(%p, %p)\n", pnWidth, pnHeight);

	global_pipe.begin_call(METH_COMP_TARGETSIZE);
	global_pipe.send(&this->objId, sizeof(uint64_t));

	global_pipe.wait_for_return();

	global_pipe.recv(pnWidth, sizeof(*pnWidth));
	global_pipe.recv(pnHeight, sizeof(*pnHeight));
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
void VRDisplayComponent::GetEyeOutputViewport( vr::EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight ) {
	global_pipe.msg("call GetEyeOutputViewport(%d, %p, %p, %p, %p)\n", eEye, pnX, pnY, pnWidth, pnHeight);

	global_pipe.begin_call(METH_COMP_EYEVIEWPORT);
	global_pipe.send(&this->objId, sizeof(uint64_t));
	global_pipe.send(&eEye, sizeof(vr::EVREye));

	global_pipe.wait_for_return();

	global_pipe.recv(pnX, sizeof(*pnX));
	global_pipe.recv(pnY, sizeof(*pnY));
	global_pipe.recv(pnWidth, sizeof(*pnWidth));
	global_pipe.recv(pnHeight, sizeof(*pnHeight));
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
void VRDisplayComponent::GetProjectionRaw( vr::EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) {
	global_pipe.msg("call GetProjectionRaw(%d, %p, %p, %p, %p)\n", eEye, pfLeft, pfRight, pfTop, pfBottom);

	global_pipe.begin_call(METH_COMP_PROJRAW);
	global_pipe.send(&this->objId, sizeof(uint64_t));
	global_pipe.send(&eEye, sizeof(vr::EVREye));

	global_pipe.wait_for_return();

	global_pipe.recv(pfLeft, sizeof(*pfLeft));
	global_pipe.recv(pfRight, sizeof(*pfRight));
	global_pipe.recv(pfTop, sizeof(*pfTop));
	global_pipe.recv(pfBottom, sizeof(*pfBottom));
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
}
vr::DistortionCoordinates_t VRDisplayComponent::ComputeDistortion( vr::EVREye eEye, float fU, float fV ) {
	global_pipe.msg("call ComputeDistortion(%d, %f, %f)\n", eEye, fU, fV);

	global_pipe.begin_call(METH_COMP_DISTORTION);
	global_pipe.send(&this->objId, sizeof(uint64_t));
	global_pipe.send(&eEye, sizeof(eEye));
	global_pipe.send(&fU, sizeof(fU));
	global_pipe.send(&fV, sizeof(fV));

	global_pipe.wait_for_return();

	vr::DistortionCoordinates_t ret;
	global_pipe.recv(&ret, sizeof(ret));
	global_pipe.return_read_channel();

	global_pipe.msg("ret\n");
	return ret;
}
bool VRDisplayComponent::ComputeInverseDistortion( vr::HmdVector2_t *pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV ) {
	STUB(global_pipe);
	return false;
}

class TrackedDeviceServerDriver : public vr::ITrackedDeviceServerDriver {
	size_t objId;

	public:
	TrackedDeviceServerDriver(size_t objId) : objId(objId) {};

	virtual EVRInitError Activate( uint32_t unObjectId );
	virtual void Deactivate();
	virtual void EnterStandby();
	virtual void *GetComponent( const char *pchComponentNameAndVersion );
	virtual void DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize );
	virtual DriverPose_t GetPose();
};

EVRInitError TrackedDeviceServerDriver::Activate( uint32_t unObjectId ) {
	global_pipe.msg("call Activate(%d)\n", unObjectId);

	global_pipe.begin_call(METH_DEV_ACTIVATE);
	global_pipe.send(&this->objId, sizeof(uint64_t));
	global_pipe.send(&unObjectId, sizeof(uint32_t));

	global_pipe.wait_for_return();

	vr::EVRInitError ret;
	global_pipe.recv(&ret, sizeof(ret));
	global_pipe.return_read_channel();

	global_pipe.msg("ret %d\n", ret);
	return ret;
}
void TrackedDeviceServerDriver::Deactivate() {
	STUB(global_pipe);
}
void TrackedDeviceServerDriver::EnterStandby() {
	STUB(global_pipe);
}
void *TrackedDeviceServerDriver::GetComponent( const char *pchComponentNameAndVersion ) {
	global_pipe.msg("call GetComponent(%s)\n", pchComponentNameAndVersion);

	global_pipe.begin_call(METH_DEV_COMPONENT);
	global_pipe.send(&this->objId, sizeof(this->objId));
	size_t nameLen = strlen(pchComponentNameAndVersion);
	global_pipe.send(&nameLen, sizeof(nameLen));
	global_pipe.send(pchComponentNameAndVersion, nameLen);

	global_pipe.wait_for_return();

	uint64_t newHandle;
	global_pipe.recv(&newHandle, sizeof(newHandle));
	global_pipe.return_read_channel();

	if(strcmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version) == 0) {
		global_pipe.msg("Create Display Component\n");
		return new VRDisplayComponent(newHandle);
	}else if(strcmp(pchComponentNameAndVersion, vr::IVRDriverDirectModeComponent_Version) == 0) {
		global_pipe.msg("Create Direct Driver Component\n");
		return new VRDriverDirect(newHandle);
	}

	global_pipe.msg("No component shim implemented, producing nullptr\n");
	return nullptr;
}
void TrackedDeviceServerDriver::DebugRequest( const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) {
	STUB(global_pipe);
}
DriverPose_t TrackedDeviceServerDriver::GetPose() {
	STUB(global_pipe);
	return {
		.result = vr::ETrackingResult::TrackingResult_Uninitialized,
	};
}

static void handler(enum PipeMethod m, void *userdata) {
	switch(m) {
	case METH_GET_INTERFACE: {
		size_t driverHandle;
		global_pipe.recv(&driverHandle, sizeof(size_t));

		uint64_t size;
		global_pipe.recv(&size, sizeof(uint64_t));
		char *buf = (char*)malloc(size + 1);
		global_pipe.recv(buf, size);
		buf[size] = '\0';
		size_t taskId = global_pipe.complete_reading_args();

		global_pipe.msg("Lookup interface %s\n", buf);

		vr::EVRInitError err;
		void *obj = ((vr::IVRDriverContext*)global_pipe.objs[driverHandle-1])->GetGenericInterface(buf, &err);

		global_pipe.msg("Interface addr %p, errcode: %d\n", obj, err);

		global_pipe.return_from_call(taskId);
		global_pipe.send_new_obj(obj);
		global_pipe.send(&err, sizeof(vr::EVRInitError));

		free(buf);
		break;
	}
	case METH_LOG: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRDriverLog *thisObj = ((vr::IVRDriverLog*)global_pipe.objs[thisHandle-1]);

		uint64_t len;
		global_pipe.recv(&len, sizeof(uint64_t));
		char *msg = (char*)malloc(len + 1);
		global_pipe.recv(msg, len);
		msg[len] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		thisObj->Log(msg);

		global_pipe.return_from_call(taskId);

		free(msg);
		break;
	}
	case METH_RES_LOAD: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRResources *thisObj = ((vr::IVRResources*)global_pipe.objs[thisHandle-1]);

		uint64_t nameLen;
		global_pipe.recv(&nameLen, sizeof(uint64_t));
		char *name = (char*)malloc(nameLen + 1);
		global_pipe.recv(name, nameLen);
		name[nameLen] = '\0';
		uint32_t bufLen;
		global_pipe.recv(&bufLen, sizeof(uint32_t));

		size_t taskId = global_pipe.complete_reading_args();

		char* buf = (char*)malloc(bufLen);
		uint32_t ret = thisObj->LoadSharedResource(name, buf, bufLen);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(uint32_t));
		global_pipe.send(buf, ret);

		free(name);
		free(buf);
		break;
	}
	case METH_RES_PATH: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRResources *thisObj = ((vr::IVRResources*)global_pipe.objs[thisHandle-1]);

		uint64_t nameLen;
		global_pipe.recv(&nameLen, sizeof(uint64_t));
		char *name = (char*)malloc(nameLen + 1);
		global_pipe.recv(name, nameLen);
		name[nameLen] = '\0';
		uint64_t dirLen;
		global_pipe.recv(&dirLen, sizeof(dirLen));
		char *dir;
		if(dirLen == 0) {
			dir = nullptr;
		} else {
			dir = (char*)malloc(dirLen + 1);
			global_pipe.recv(dir, dirLen);
			dir[dirLen] = '\0';
		}
		uint32_t bufLen;
		global_pipe.recv(&bufLen, sizeof(uint32_t));

		size_t taskId = global_pipe.complete_reading_args();

		char *buf = (char*)malloc(bufLen);
		uint32_t ret = thisObj->GetResourceFullPath(name, dir, buf, bufLen);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(uint32_t));
		global_pipe.send(buf, std::min(ret, bufLen));
		
		free(name);
		free(buf);
		break;
	}
	case METH_SETS_GBOOL: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRSettings *thisObj = ((vr::IVRSettings*)global_pipe.objs[thisHandle-1]);

		uint64_t sectionLen;
		global_pipe.recv(&sectionLen, sizeof(sectionLen));
		char *section = (char*)malloc(sectionLen + 1);
		global_pipe.recv(section, sectionLen);
		section[sectionLen] = '\0';

		uint64_t keyLen;
		global_pipe.recv(&keyLen, sizeof(keyLen));
		char *key = (char*)malloc(keyLen + 1);
		global_pipe.recv(key, keyLen);
		key[keyLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::EVRSettingsError err;
		uint8_t ret = thisObj->GetBool(section, key, &err);
		global_pipe.msg("Calling getbool with %s, %s = %d\n", section, key, ret);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&err, sizeof(err));

		free(section);
		free(key);
		break;
	}
	case METH_SETS_GINT: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRSettings *thisObj = ((vr::IVRSettings*)global_pipe.objs[thisHandle-1]);

		uint64_t sectionLen;
		global_pipe.recv(&sectionLen, sizeof(sectionLen));
		char *section = (char*)malloc(sectionLen + 1);
		global_pipe.recv(section, sectionLen);
		section[sectionLen] = '\0';

		uint64_t keyLen;
		global_pipe.recv(&keyLen, sizeof(keyLen));
		char *key = (char*)malloc(keyLen + 1);
		global_pipe.recv(key, keyLen);
		key[keyLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::EVRSettingsError err;
		int32_t ret = thisObj->GetInt32(section, key, &err);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&err, sizeof(err));

		free(section);
		free(key);
		break;
	}
	case METH_SETS_GFLT: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRSettings *thisObj = ((vr::IVRSettings*)global_pipe.objs[thisHandle-1]);

		uint64_t sectionLen;
		global_pipe.recv(&sectionLen, sizeof(sectionLen));
		char *section = (char*)malloc(sectionLen + 1);
		global_pipe.recv(section, sectionLen);
		section[sectionLen] = '\0';

		uint64_t keyLen;
		global_pipe.recv(&keyLen, sizeof(keyLen));
		char *key = (char*)malloc(keyLen + 1);
		global_pipe.recv(key, keyLen);
		key[keyLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::EVRSettingsError err;
		float ret = thisObj->GetFloat(section, key, &err);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&err, sizeof(err));

		free(section);
		free(key);
		break;
	}
	case METH_SETS_GSTR: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRSettings *thisObj = ((vr::IVRSettings*)global_pipe.objs[thisHandle-1]);

		uint64_t sectionLen;
		global_pipe.recv(&sectionLen, sizeof(sectionLen));
		char *section = (char*)malloc(sectionLen + 1);
		global_pipe.recv(section, sectionLen);
		section[sectionLen] = '\0';

		uint64_t keyLen;
		global_pipe.recv(&keyLen, sizeof(keyLen));
		char *key = (char*)malloc(keyLen + 1);
		global_pipe.recv(key, keyLen);
		key[keyLen] = '\0';

		uint32_t valueLen;
		global_pipe.recv(&valueLen, sizeof(valueLen));
		char *value = (char*)malloc(valueLen);

		size_t taskId = global_pipe.complete_reading_args();
		global_pipe.msg("Task ID is = %d\n", taskId);

		vr::EVRSettingsError err;
		thisObj->GetString(section, key, value, valueLen, &err);

		global_pipe.return_from_call(taskId);
		global_pipe.send(value, valueLen);
		global_pipe.send(&err, sizeof(err));

		free(section);
		free(key);
		free(value);
		break;
	}
	case METH_PATH_WRITE: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRPaths *thisObj = ((vr::IVRPaths*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t root;
		global_pipe.recv(&root, sizeof(uint64_t));

		uint32_t entries;
		global_pipe.recv(&entries, sizeof(entries));

		vr::PathWrite_t *batch = (vr::PathWrite_t*)malloc(sizeof(vr::PathWrite_t) * entries);
		global_pipe.msg("WritePathBatch(%ld, %p, %d)\n", root, batch, entries);
		for(uint64_t i = 0; i < entries; i++) {
			vr::PathWrite_t *it =  &batch[i];
			global_pipe.recv(&it->ulPath, sizeof(it->ulPath));
			global_pipe.recv(&it->writeType, sizeof(it->writeType));
			global_pipe.recv(&it->eSetError, sizeof(it->eSetError));
			global_pipe.recv(&it->unBufferSize, sizeof(it->unBufferSize));
			it->pvBuffer = malloc(it->unBufferSize);
			global_pipe.recv(it->pvBuffer, it->unBufferSize);

			// This is just me being lazy
			it->pszPath = nullptr;

			// @HACK This is a pretty bad hack for getting steam to forward
			// connections to the UDP port of the driver. As part of starting
			// up the driver has to handshake with some internal steam api and
			// transfer a little bit of shared state (the port the driver
			// listens on an some encryption key). In an older driver that used
			// to happen via the driver directly invoking a URL handler, but
			// they seem to have moved that into the vrserver component.
			// I can't get the vrserver to actually trigger that call so I'm
			// just doing it myself. If we could get the vrserver to invoke it
			// for us, that would be way nicer than what we had before.
			char pathStr[27];
			uint32_t pathStrLen = 0;
			if(thisObj->HandleToString(it->ulPath, pathStr, sizeof(pathStr), &pathStrLen) == ETrackedPropertyError::TrackedProp_Success) {
				if(pathStrLen == 27 && strcmp(pathStr, "/steam/vr_connection_ready") == 0) {
					char cmd[512];
					sprintf(cmd, "xdg-open \'steam://vr_connection_ready/%.*s\'", it->unBufferSize, (char*)it->pvBuffer);
					global_pipe.msg("Connection Ready Hack: %s\n", cmd);
					system(cmd);
				}
			}
		}

		size_t taskId = global_pipe.complete_reading_args();

		vr::ETrackedPropertyError ret = thisObj->WritePathBatch(root, batch, entries);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));

		for(uint64_t i = 0; i < entries; i++) {
			vr::PathWrite_t *it =  &batch[i];
			global_pipe.send(&it->unTag, sizeof(it->unTag));
			global_pipe.send(&it->eError, sizeof(it->eError));

			free(batch[i].pvBuffer);
		}
		free(batch);
		break;
	}
	case METH_PATH_READ: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRPaths *thisObj = ((vr::IVRPaths*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t root;
		global_pipe.recv(&root, sizeof(uint64_t));

		uint32_t entries;
		global_pipe.recv(&entries, sizeof(entries));

		vr::PathRead_t *batch = (vr::PathRead_t*)malloc(sizeof(vr::PathRead_t) * entries);
		for(uint64_t i = 0; i < entries; i++) {
			vr::PathRead_t *readStruct =  &batch[i];
			global_pipe.recv(&readStruct->ulPath, sizeof(readStruct->ulPath));
			global_pipe.recv(&readStruct->unBufferSize, sizeof(readStruct->unBufferSize));
			readStruct->pvBuffer = malloc(readStruct->unBufferSize);
			// This is just me being lazy
			readStruct->pszPath = nullptr;
		}
		size_t taskId = global_pipe.complete_reading_args();

		vr::PathHandle_t handle;
		vr::ETrackedPropertyError ret = thisObj->ReadPathBatch(root, batch, entries);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));

		for(uint64_t i = 0; i < entries; i++) {
			vr::PathRead_t *readStruct =  &batch[i];
			global_pipe.send(&readStruct->unTag, sizeof(readStruct->unTag));
			global_pipe.send(readStruct->pvBuffer, readStruct->unBufferSize);
			global_pipe.send(&readStruct->unRequiredBufferSize, sizeof(readStruct->unRequiredBufferSize));
			global_pipe.send(&readStruct->eError, sizeof(readStruct->eError));

			free(batch[i].pvBuffer);
		}
		free(batch);
		break;
	}
	case METH_PATH_S2H: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRPaths *thisObj = ((vr::IVRPaths*)global_pipe.objs[thisHandle-1]);

		uint64_t pathLen;
		global_pipe.recv(&pathLen, sizeof(pathLen));
		char *path = (char*)malloc(pathLen + 1);
		global_pipe.recv(path, pathLen);
		path[pathLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::PathHandle_t handle;
		vr::ETrackedPropertyError ret = thisObj->StringToHandle(&handle, path);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&handle, sizeof(handle));

		free(path);
		break;
	}
	case METH_PROP_READ: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRProperties *thisObj = ((vr::IVRProperties*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t root;
		global_pipe.recv(&root, sizeof(uint64_t));

		uint32_t entries;
		global_pipe.recv(&entries, sizeof(entries));

		vr::PropertyRead_t *batch = (vr::PropertyRead_t*)malloc(sizeof(vr::PropertyRead_t) * entries);
		for(uint64_t i = 0; i < entries; i++) {
			vr::PropertyRead_t *it =  &batch[i];
			global_pipe.recv(&it->prop, sizeof(it->prop));
			global_pipe.recv(&it->unBufferSize, sizeof(it->unBufferSize));
			it->pvBuffer = malloc(it->unBufferSize);
			global_pipe.recv(&it->unTag, sizeof(it->unTag));
		}
		size_t taskId = global_pipe.complete_reading_args();

		vr::ETrackedPropertyError ret = thisObj->ReadPropertyBatch(root, batch, entries);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));

		for(uint64_t i = 0; i < entries; i++) {
			vr::PropertyRead_t *it =  &batch[i];
			global_pipe.send(&it->unTag, sizeof(it->unTag));
			global_pipe.send(it->pvBuffer, it->unBufferSize);
			global_pipe.send(&it->unRequiredBufferSize, sizeof(it->unRequiredBufferSize));
			global_pipe.send(&it->eError, sizeof(it->eError));
			global_pipe.msg("    {%d %.*s %d %d}\n", it->unTag, it->unBufferSize, it->pvBuffer, it->unRequiredBufferSize, it->eError);

			free(batch[i].pvBuffer);
		}
		global_pipe.msg("ret %d\n", ret);
		free(batch);
		break;
	}
	case METH_PROP_WRITE: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRProperties *thisObj = ((vr::IVRProperties*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t root;
		global_pipe.recv(&root, sizeof(uint64_t));

		uint32_t entries;
		global_pipe.recv(&entries, sizeof(entries));

		vr::PropertyWrite_t *batch = (vr::PropertyWrite_t*)malloc(sizeof(vr::PropertyWrite_t) * entries);
		for(uint64_t i = 0; i < entries; i++) {
			vr::PropertyWrite_t *it =  &batch[i];
			global_pipe.recv(&it->prop, sizeof(it->prop));
			global_pipe.recv(&it->writeType, sizeof(it->writeType));
			global_pipe.recv(&it->eSetError, sizeof(it->eSetError));
			global_pipe.recv(&it->unBufferSize, sizeof(it->unBufferSize));
			it->pvBuffer = malloc(it->unBufferSize);
			global_pipe.recv(it->pvBuffer, it->unBufferSize);
			global_pipe.recv(&it->unTag, sizeof(it->unTag));
		}
		size_t taskId = global_pipe.complete_reading_args();

		vr::ETrackedPropertyError ret = thisObj->WritePropertyBatch(root, batch, entries);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));

		for(uint64_t i = 0; i < entries; i++) {
			vr::PropertyWrite_t *it =  &batch[i];
			global_pipe.send(&it->unTag, sizeof(it->unTag));
			global_pipe.send(&it->eError, sizeof(it->eError));

			free(batch[i].pvBuffer);
		}
		global_pipe.msg("ret %d\n", ret);
		free(batch);
		break;
	}
	case METH_PROP_TRANS: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRProperties *thisObj = ((vr::IVRProperties*)global_pipe.objs[thisHandle-1]);

		vr::TrackedDeviceIndex_t dev;
		global_pipe.recv(&dev, sizeof(dev));

		size_t taskId = global_pipe.complete_reading_args();

		vr::PropertyContainerHandle_t ret = thisObj->TrackedDeviceToPropertyContainer(dev);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_SERVER_DEVADD: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		uint64_t serialLen;
		global_pipe.recv(&serialLen, sizeof(uint64_t));
		char *serial = (char*)malloc(serialLen + 1);
		global_pipe.recv(serial, serialLen);
		serial[serialLen] = '\0';
		vr::ETrackedDeviceClass deviceClass;
		global_pipe.recv(&deviceClass, sizeof(deviceClass));

		size_t driverHandle;
		global_pipe.recv(&driverHandle, sizeof(driverHandle));
		vr::ITrackedDeviceServerDriver *deviceDriver = new TrackedDeviceServerDriver(driverHandle);

		size_t taskId = global_pipe.complete_reading_args();

		uint8_t ret = thisObj->TrackedDeviceAdded(serial, deviceClass, deviceDriver);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));

		free(serial);
		break;
	}
	case METH_INPUT_CBOOL: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRDriverInput *thisObj = ((vr::IVRDriverInput*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t container;
		global_pipe.recv(&container, sizeof(container));
		size_t nameLen;
		global_pipe.recv(&nameLen, sizeof(nameLen));
		char *name = (char*)malloc(nameLen + 1);
		global_pipe.recv(name, nameLen);
		name[nameLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::VRInputComponentHandle_t handle;
		vr::EVRInputError ret = thisObj->CreateBooleanComponent(container, name, &handle);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&handle, sizeof(handle));

		free(name);
		break;
	}
	case METH_INPUT_UBOOL: {
		uint64_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(thisHandle));
		vr::IVRDriverInput *thisObj = ((vr::IVRDriverInput*)global_pipe.objs[thisHandle-1]);

		vr::VRInputComponentHandle_t handle;
		global_pipe.recv(&handle, sizeof(handle));
		bool newValue;
		global_pipe.recv(&newValue, sizeof(newValue));
		double timeOffset;
		global_pipe.recv(&timeOffset, sizeof(timeOffset));

		size_t taskId = global_pipe.complete_reading_args();

		vr::EVRInputError ret = thisObj->UpdateBooleanComponent(handle, newValue, timeOffset);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_INPUT_CSCALAR: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRDriverInput *thisObj = ((vr::IVRDriverInput*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t container;
		global_pipe.recv(&container, sizeof(container));
		size_t nameLen;
		global_pipe.recv(&nameLen, sizeof(nameLen));
		char *name = (char*)malloc(nameLen + 1);
		global_pipe.recv(name, nameLen);
		name[nameLen] = '\0';
		vr::EVRScalarType type;
		global_pipe.recv(&type, sizeof(type));
		vr::EVRScalarUnits units;
		global_pipe.recv(&units, sizeof(units));

		size_t taskId = global_pipe.complete_reading_args();

		vr::VRInputComponentHandle_t handle;
		vr::EVRInputError ret = thisObj->CreateScalarComponent(container, name, &handle, type, units);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&handle, sizeof(handle));

		free(name);
		break;
	}
	case METH_INPUT_USCALAR: {
		uint64_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(thisHandle));
		vr::IVRDriverInput *thisObj = ((vr::IVRDriverInput*)global_pipe.objs[thisHandle-1]);

		vr::VRInputComponentHandle_t handle;
		global_pipe.recv(&handle, sizeof(handle));
		float newValue;
		global_pipe.recv(&newValue, sizeof(newValue));
		double timeOffset;
		global_pipe.recv(&timeOffset, sizeof(timeOffset));

		size_t taskId = global_pipe.complete_reading_args();

		vr::EVRInputError ret = thisObj->UpdateScalarComponent(handle, newValue, timeOffset);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_INPUT_CHAPTIC: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(size_t));
		vr::IVRDriverInput *thisObj = ((vr::IVRDriverInput*)global_pipe.objs[thisHandle-1]);

		vr::PropertyContainerHandle_t container;
		global_pipe.recv(&container, sizeof(container));
		size_t nameLen;
		global_pipe.recv(&nameLen, sizeof(nameLen));
		char *name = (char*)malloc(nameLen + 1);
		global_pipe.recv(name, nameLen);
		name[nameLen] = '\0';

		size_t taskId = global_pipe.complete_reading_args();

		vr::VRInputComponentHandle_t handle;
		vr::EVRInputError ret = thisObj->CreateHapticComponent(container, name, &handle);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&handle, sizeof(handle));

		free(name);
		break;
	}
	case METH_MB_UNDOC1: {
		uint64_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(thisHandle));
		vr::IVRMailbox *thisObj = ((vr::IVRMailbox*)global_pipe.objs[thisHandle-1]);

		uint64_t bufSize;
		global_pipe.recv(&bufSize, sizeof(bufSize));
		char *buf = (char*)malloc(bufSize + 1);
		global_pipe.recv(buf, bufSize);
		buf[bufSize] = '\0';
		uint32_t csize;
		global_pipe.recv(&csize, sizeof(csize));
		char *c = (char*)malloc(csize);
		
		size_t taskId = global_pipe.complete_reading_args();

		vr::vrmb_typea outVal = 0;
		vr::vrmb_typeb ret = thisObj->undoc1(buf, &outVal, c, csize);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&outVal, sizeof(outVal));
		global_pipe.send(c, csize);

		free(buf);
		free(c);
		break;
	}
	case METH_MB_UNDOC2: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRMailbox *thisObj = ((vr::IVRMailbox*)global_pipe.objs[thisHandle-1]);

		vr::vrmb_typea arg1;
		global_pipe.recv(&arg1, sizeof(uint64_t));

		size_t taskId = global_pipe.complete_reading_args();

		vr::vrmb_typeb ret = thisObj->undoc2(arg1);

		global_pipe.return_from_call(taskId);
		global_pipe.msg("undoc2 ret %d\n", ret);
		global_pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_MB_UNDOC3: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRMailbox *thisObj = ((vr::IVRMailbox*)global_pipe.objs[thisHandle-1]);

		vr::vrmb_typea arg1;
		global_pipe.recv(&arg1, sizeof(uint64_t));

		uint64_t blen;
		global_pipe.recv(&blen, sizeof(blen));
		char *b = (char*)malloc(blen);
		global_pipe.recv(b, blen);

		uint64_t clen;
		global_pipe.recv(&clen, sizeof(clen));
		char *c = (char*)malloc(clen);
		global_pipe.recv(c, clen);

		bool d;
		global_pipe.recv(&d, sizeof(d));
		
		size_t taskId = global_pipe.complete_reading_args();

		vr::vrmb_typeb ret = thisObj->undoc3(arg1, b, c, d);

		global_pipe.return_from_call(taskId);
		global_pipe.msg("undoc3 ret %d\n", ret);
		global_pipe.send(&ret, sizeof(ret));

		free(b);
		free(c);
		break;
	}
	case METH_MB_UNDOC4: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRMailbox *thisObj = ((vr::IVRMailbox*)global_pipe.objs[thisHandle-1]);

		vr::vrmb_typea arg1;
		global_pipe.recv(&arg1, sizeof(uint64_t));
		uint32_t bufSize;
		global_pipe.recv(&bufSize, sizeof(uint32_t));
		char *buf = (char*)malloc(bufSize);
		
		size_t taskId = global_pipe.complete_reading_args();

		uint32_t outSize = 0;
		vr::vrmb_typeb ret = thisObj->undoc4(arg1, buf, bufSize, &outSize);

		global_pipe.return_from_call(taskId);
		global_pipe.msg("undoc4 ret %d %ld %p %ld\n", ret, outSize, buf, bufSize);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&outSize, sizeof(outSize));
		global_pipe.send(buf, bufSize);

		free(buf);
		break;
	}
	case METH_SERVER_POSE: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		uint32_t device;
		global_pipe.recv(&device, sizeof(device));

		vr::DriverPose_t pose;
		global_pipe.recv(&pose, sizeof(pose));
		
		size_t taskId = global_pipe.complete_reading_args();

		thisObj->TrackedDevicePoseUpdated(device, pose, sizeof(pose));

		global_pipe.return_from_call(taskId);
		break;
	}
	case METH_SERVER_VSYNC: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		double timeOffset;
		global_pipe.recv(&timeOffset, sizeof(timeOffset));
		
		size_t taskId = global_pipe.complete_reading_args();

		thisObj->VsyncEvent(timeOffset);

		global_pipe.return_from_call(taskId);
		break;
	}
	case METH_SERVER_VENDOR: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		uint32_t dev;
		global_pipe.recv(&dev, sizeof(dev));
		vr::EVREventType type;
		global_pipe.recv(&type, sizeof(type));
		vr::VREvent_Data_t eventData;
		global_pipe.recv(&eventData, sizeof(eventData));
		double timeOffset;
		global_pipe.recv(&timeOffset, sizeof(timeOffset));
		
		size_t taskId = global_pipe.complete_reading_args();

		thisObj->VendorSpecificEvent(dev, type, eventData, timeOffset);

		global_pipe.return_from_call(taskId);
		break;
	}
	case METH_SERVER_POLL: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		uint32_t eventSize;
		global_pipe.recv(&eventSize, sizeof(eventSize));
		vr::VREvent_t *buf = (vr::VREvent_t*)malloc(eventSize);
		
		size_t taskId = global_pipe.complete_reading_args();

		bool ret = thisObj->PollNextEvent(buf, eventSize);

		global_pipe.return_from_call(taskId);
		global_pipe.send(&ret, sizeof(ret));
		global_pipe.send(&buf, eventSize);

		free(buf);
		break;
	}
	case METH_SERVER_PROJ: {
		size_t thisHandle;
		global_pipe.recv(&thisHandle, sizeof(uint64_t));
		vr::IVRServerDriverHost *thisObj = ((vr::IVRServerDriverHost*)global_pipe.objs[thisHandle-1]);

		uint32_t dev;
		global_pipe.recv(&dev, sizeof(dev));
		vr::HmdRect2_t left;
		global_pipe.recv(&left, sizeof(left));
		vr::HmdRect2_t right;
		global_pipe.recv(&right, sizeof(right));
		
		size_t taskId = global_pipe.complete_reading_args();

		thisObj->SetDisplayProjectionRaw(dev, left, right);

		global_pipe.return_from_call(taskId);
		break;
	}
	default: {
		global_pipe.msg("Unhandled method\n");
		abort();
	}
	}
}

void taskHandler() {
	char tname[16];
	pthread_getname_np(pthread_self(), tname, 16);
	global_pipe.msg("Hello from the task thread: %s\n", tname);
	global_pipe.dispatch_requests(nullptr);
}

std::mutex global_lock;
extern "C" __attribute__((visibility("default"))) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
	{
		std::unique_lock lock(global_lock);
		if(global_pipe.log == nullptr) {
			global_pipe._reinit(false, handler);
			std::thread taskThread(taskHandler);
			taskThread.detach();
		}
	}
	char tname[16];
	pthread_getname_np(pthread_self(), tname, 16);
	global_pipe.msg("Main driver thread %s\n", tname);
	global_pipe.msg("Looking for interface %s\n", pInterfaceName);

	if (0 == strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName)) {
		global_pipe.begin_call(METH_DRIVER_FACTORY);

		size_t nameLen = strlen(pInterfaceName);
		global_pipe.send(&nameLen, sizeof(size_t));
		global_pipe.send(pInterfaceName, nameLen);

		global_pipe.wait_for_return();
		handle retObj;
		global_pipe.recv(&retObj, sizeof(handle));
		global_pipe.recv(pReturnCode, sizeof(int));
		global_pipe.return_read_channel();

		global_pipe.msg("Got back object %ld\n", retObj);

		device_provider.handle = retObj;

		return &device_provider;
	}

	if (pReturnCode) {
		*pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
	}

	return nullptr;
}
