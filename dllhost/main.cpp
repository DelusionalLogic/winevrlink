#include "ipc.h"
#include <cassert>
#include <openvr_driver.h>
#include <iostream>
#include <windows.h>
#include <wine/windows/d3d11.h>
#include <winternl.h>
#include <stdio.h>
#include <dirent.h>
#include <d3d11_4.h>

struct DriverState {
	Pipe pipe;
	HINSTANCE hDLL;
};


typedef unsigned int obj_handle_t;

static inline HANDLE wine_server_ptr_handle( obj_handle_t handle )
{
    return (HANDLE)(INT_PTR)(int)handle;
}

static inline obj_handle_t wine_server_obj_handle( HANDLE handle )
{
    if ((int)(INT_PTR)handle != (INT_PTR)handle) return 0xfffffff0;  /* some invalid handle */
    return (INT_PTR)handle;
}

struct DxvkSharedTextureMetadata {
	UINT             Width;
	UINT             Height;
	UINT             MipLevels;
	UINT             ArraySize;
	DXGI_FORMAT      Format;
	DXGI_SAMPLE_DESC SampleDesc;
	D3D11_USAGE      Usage;
	UINT             BindFlags;
	UINT             CPUAccessFlags;
	UINT             MiscFlags;
	D3D11_TEXTURE_LAYOUT TextureLayout;
};

#define IOCTL_SHARED_GPU_RESOURCE_GET_METADATA           CTL_CODE(FILE_DEVICE_VIDEO, 5, METHOD_BUFFERED, FILE_READ_ACCESS)

bool getSharedMetadata(HANDLE handle, void *buf, uint32_t bufSize, uint32_t *metadataSize) {
	DWORD retSize;
	bool ret = DeviceIoControl(handle, IOCTL_SHARED_GPU_RESOURCE_GET_METADATA, NULL, 0, buf, bufSize, &retSize, NULL);
	if (metadataSize)
		*metadataSize = retSize;
	return ret;
}

#define IOCTL_SHARED_GPU_RESOURCE_OPEN             CTL_CODE(FILE_DEVICE_VIDEO, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

struct shared_resource_open
{
    obj_handle_t kmt_handle;
    WCHAR name[1];
};

struct shared_resource_info
{
    UINT64 resource_size;
};

static inline void init_unicode_string( UNICODE_STRING *str, const WCHAR *data )
{
    str->Length = lstrlenW(data) * sizeof(WCHAR);
    str->MaximumLength = str->Length + sizeof(WCHAR);
    str->Buffer = (WCHAR *)data;
}

static HANDLE open_shared_resource(HANDLE kmt_handle, LPCWSTR name)
{
    static const WCHAR shared_gpu_resourceW[] = {'\\','?','?','\\','S','h','a','r','e','d','G','p','u','R','e','s','o','u','r','c','e',0};
    UNICODE_STRING shared_gpu_resource_us;
    struct shared_resource_open *inbuff;
    HANDLE shared_resource;
    OBJECT_ATTRIBUTES attr;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;
    DWORD in_size;

    init_unicode_string(&shared_gpu_resource_us, shared_gpu_resourceW);

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.Attributes = 0;
    attr.ObjectName = &shared_gpu_resource_us;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    if ((status = NtCreateFile(&shared_resource, GENERIC_READ | GENERIC_WRITE, &attr, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0)))
    {
        printf("Failed to load open a shared resource handle, status %#lx.\n", (long int)status);
        return INVALID_HANDLE_VALUE;
    }

    in_size = sizeof(*inbuff) + (name ? lstrlenW(name) * sizeof(WCHAR) : 0);
    inbuff = (struct shared_resource_open*)calloc(1, in_size);
    inbuff->kmt_handle = wine_server_obj_handle(kmt_handle);
    if (name)
        lstrcpyW(&inbuff->name[0], name);

    status = NtDeviceIoControlFile(shared_resource, NULL, NULL, NULL, &iosb, IOCTL_SHARED_GPU_RESOURCE_OPEN,
            inbuff, in_size, NULL, 0);

    free(inbuff);

    if (status)
    {
        printf("Failed to open video resource, status %#lx.\n", (long int)status);
        NtClose(shared_resource);
        return INVALID_HANDLE_VALUE;
    }

    return shared_resource;
}


#define IOCTL_SHARED_GPU_RESOURCE_GET_UNIX_RESOURCE           CTL_CODE(FILE_DEVICE_VIDEO, 3, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_SHARED_GPU_RESOURCE_GET_DMA_RESOURCE           CTL_CODE(FILE_DEVICE_VIDEO, 8, METHOD_BUFFERED, FILE_READ_ACCESS)

extern "C" NTSYSAPI NTSTATUS CDECL wine_server_handle_to_fd( HANDLE handle, unsigned int access, int *unix_fd, unsigned int *options );
static int get_shared_resource_fd(HANDLE shared_resource, struct DriverState *state) {
	IO_STATUS_BLOCK iosb;
	uint32_t unix_resource;
	NTSTATUS status;
	int ret;

	shared_resource = open_shared_resource( shared_resource, nullptr );
	if(shared_resource == INVALID_HANDLE_VALUE) {
		return -1;
	}

	DxvkSharedTextureMetadata metadata;
	getSharedMetadata(shared_resource, &metadata, sizeof(metadata), nullptr);
	state->pipe.msg("    0x%08X -> %d %d %d %d %d\n", shared_resource, metadata.Width, metadata.Height, metadata.Format, metadata.SampleDesc, metadata.TextureLayout);

	if (NtDeviceIoControlFile(shared_resource, NULL, NULL, NULL, &iosb, IOCTL_SHARED_GPU_RESOURCE_GET_DMA_RESOURCE,
				NULL, 0, &unix_resource, sizeof(unix_resource)))
		return -1;

	status = wine_server_handle_to_fd(wine_server_ptr_handle(unix_resource), FILE_READ_DATA, &ret, NULL);
	NtClose(wine_server_ptr_handle(unix_resource));
	return status == 0 ? ret : -1;
}

#define STUB() \
do { \
	std::cerr << "Unimplemented stub " << __PRETTY_FUNCTION__ << std::endl; \
	std::flush(std::cerr); \
}while(0)

struct NullObject{
	void* vtable;
	const char* name;
};

typedef void* (__attribute__((ms_abi)) *HmdDriverFactoryPtr)(const char*, int*);

__attribute__((noreturn)) MSABI void *uhoh(struct NullObject *obj) {
	std::cerr << "Uh Oh: " << obj->name << std::endl;
	std::flush(std::cerr);

	abort();
}

void *nullspot[] = {
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
	(void*)&uhoh,
};

class VRDriverInput : public vr::IVRDriverInput {
	uint64_t objId;
	struct DriverState *state;
public:
	VRDriverInput(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {};

public:
	MSABI virtual vr::EVRInputError CreateBooleanComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle );
	MSABI virtual vr::EVRInputError UpdateBooleanComponent( vr::VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset );
	MSABI virtual vr::EVRInputError CreateScalarComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle, vr::EVRScalarType eType, vr::EVRScalarUnits eUnits );
	MSABI virtual vr::EVRInputError UpdateScalarComponent( vr::VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset );
	MSABI virtual vr::EVRInputError CreateHapticComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle );
	MSABI virtual vr::EVRInputError CreateSkeletonComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, const char *pchSkeletonPath, const char *pchBasePosePath, vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel, const vr::VRBoneTransform_t *pGripLimitTransforms, uint32_t unGripLimitTransformCount, vr::VRInputComponentHandle_t *pHandle );
	MSABI virtual vr::EVRInputError UpdateSkeletonComponent( vr::VRInputComponentHandle_t ulComponent, vr::EVRSkeletalMotionRange eMotionRange, const vr::VRBoneTransform_t *pTransforms, uint32_t unTransformCount );
};

MSABI vr::EVRInputError VRDriverInput::CreateBooleanComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle ) {
	state->pipe.msg("call CreateBooleanComponent(%ld, %s, %p)\n", ulContainer, pchName, pHandle);
	state->pipe.begin_call(METH_INPUT_CBOOL);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulContainer, sizeof(ulContainer));
	size_t nameLen = strlen(pchName);
	state->pipe.send(&nameLen, sizeof(nameLen));
	state->pipe.send(pchName, nameLen);

	state->pipe.wait_for_return();

	vr::EVRInputError ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(pHandle, sizeof(*pHandle));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d, %d\n", ret, *pHandle);
	return ret;
}
MSABI vr::EVRInputError VRDriverInput::UpdateBooleanComponent( vr::VRInputComponentHandle_t ulComponent, bool bNewValue, double fTimeOffset ) {
	state->pipe.msg("call UpdateBooleanComponent(%ld, %d, %lf)\n", ulComponent, bNewValue, fTimeOffset);
	state->pipe.begin_call(METH_INPUT_UBOOL);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulComponent, sizeof(ulComponent));
	state->pipe.send(&bNewValue, sizeof(bNewValue));
	state->pipe.send(&fTimeOffset, sizeof(fTimeOffset));

	state->pipe.wait_for_return();

	vr::EVRInputError ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d\n", ret);
	return ret;
}
MSABI vr::EVRInputError VRDriverInput::CreateScalarComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle, vr::EVRScalarType eType, vr::EVRScalarUnits eUnits ) {
	STUB();
	return vr::EVRInputError::VRInputError_NoData;
}
MSABI vr::EVRInputError VRDriverInput::UpdateScalarComponent( vr::VRInputComponentHandle_t ulComponent, float fNewValue, double fTimeOffset ) {
	STUB();
	return vr::EVRInputError::VRInputError_NoData;
}
MSABI vr::EVRInputError VRDriverInput::CreateHapticComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, vr::VRInputComponentHandle_t *pHandle ) {
	STUB();
	return vr::EVRInputError::VRInputError_NoData;
}
MSABI vr::EVRInputError VRDriverInput::CreateSkeletonComponent( vr::PropertyContainerHandle_t ulContainer, const char *pchName, const char *pchSkeletonPath, const char *pchBasePosePath, vr::EVRSkeletalTrackingLevel eSkeletalTrackingLevel, const vr::VRBoneTransform_t *pGripLimitTransforms, uint32_t unGripLimitTransformCount, vr::VRInputComponentHandle_t *pHandle ) {
	STUB();
	return vr::EVRInputError::VRInputError_NoData;
}
MSABI vr::EVRInputError VRDriverInput::UpdateSkeletonComponent( vr::VRInputComponentHandle_t ulComponent, vr::EVRSkeletalMotionRange eMotionRange, const vr::VRBoneTransform_t *pTransforms, uint32_t unTransformCount ) {
	STUB();
	return vr::EVRInputError::VRInputError_NoData;
}

class VRMailbox : public vr::IVRMailbox {
	uint64_t objId;
	struct DriverState *state;
public:
	VRMailbox(struct DriverState *state, uint64_t objId);

	MSABI virtual vr::vrmb_typeb undoc1(const char *a, vr::vrmb_typea *b, char *c, uint32_t d);
	MSABI virtual vr::vrmb_typeb undoc2(vr::vrmb_typea a);
	MSABI virtual vr::vrmb_typeb undoc3(vr::vrmb_typea a, const char *b, const char *c);
	MSABI virtual vr::vrmb_typeb undoc4(vr::vrmb_typea a, char *b, uint32_t c, uint32_t *d);
};

VRMailbox::VRMailbox(struct DriverState *state, uint64_t objId) : state(state), objId(objId) { }

MSABI vr::vrmb_typeb VRMailbox::undoc1(const char *a, vr::vrmb_typea *b, char *c, uint32_t d) {
	state->pipe.msg("call undoc1(%s, %p, %s, %d)\n", a, b, c, d);
	state->pipe.begin_call(METH_MB_UNDOC1);
	state->pipe.send(&objId, sizeof(uint64_t));
	uint64_t alen = strlen(a);
	state->pipe.send(&alen, sizeof(uint64_t));
	state->pipe.send(a, alen);
	state->pipe.send(&d, sizeof(d));

	state->pipe.wait_for_return();

	vr::vrmb_typeb ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(b, sizeof(*b));
	state->pipe.recv(c, d);
	state->pipe.return_read_channel();
	return ret;
}
MSABI vr::vrmb_typeb VRMailbox::undoc2(vr::vrmb_typea a) {
	STUB();
	return vr::vrmb_typeb::valueb;
}
MSABI vr::vrmb_typeb VRMailbox::undoc3(vr::vrmb_typea a, const char *b, const char *c) {
	STUB();
	return vr::vrmb_typeb::valueb;
}
MSABI vr::vrmb_typeb VRMailbox::undoc4(vr::vrmb_typea a, char *b, uint32_t c, uint32_t *d) {
	state->pipe.msg("call undoc4(%d, %p, %d, %p)\n", a, b, c, d);
	state->pipe.begin_call(METH_MB_UNDOC4);
	state->pipe.send(&objId, sizeof(uint64_t));
	state->pipe.send(&a, sizeof(uint64_t));
	state->pipe.send(&c, sizeof(uint32_t));

	state->pipe.wait_for_return();

	vr::vrmb_typeb ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(d, sizeof(uint32_t));
	state->pipe.msg("undoc4 ret %d %ld\n", ret, c);
	state->pipe.recv(b, c);
	state->pipe.msg("undoc4 ret %p\n", b);
	state->pipe.return_read_channel();
	return ret;
}

class VRPaths : public vr::IVRPaths {
	uint64_t objId;
	struct DriverState *state;
public:
	VRPaths(struct DriverState *state, uint64_t objId);

	MSABI virtual vr::ETrackedPropertyError ReadPathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathRead_t * pBatch, uint32_t unBatchEntryCount);
	MSABI virtual vr::ETrackedPropertyError WritePathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathWrite_t * pBatch, uint32_t unBatchEntryCount);
	MSABI virtual vr::ETrackedPropertyError StringToHandle(vr::PathHandle_t * pHandle, char * pchPath);
	MSABI virtual vr::ETrackedPropertyError HandleToString(vr::PathHandle_t pHandle, char * pchBuffer, uint32_t unBufferSize, uint32_t * punBufferSizeUsed);
};

VRPaths::VRPaths(struct DriverState *state, uint64_t objId) : state(state), objId(objId) { }

MSABI vr::ETrackedPropertyError VRPaths::ReadPathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathRead_t * pBatch, uint32_t unBatchEntryCount) {
	state->pipe.msg("call ReadPathBatch(%d, %p, %d)\n", ulRootHandle, pBatch, unBatchEntryCount);

	state->pipe.begin_call(METH_PATH_READ);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulRootHandle, sizeof(uint64_t));

	state->pipe.send(&unBatchEntryCount, sizeof(unBatchEntryCount));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathRead_t *it =  &pBatch[i];
		state->pipe.send(&it->ulPath, sizeof(it->ulPath));
		state->pipe.send(&it->unBufferSize, sizeof(it->unBufferSize));
		if(it->pszPath != nullptr) {
			state->pipe.msg("The NULL alsoPath assumption did not hold %s", it->pszPath);
		}
	}

	state->pipe.wait_for_return();

	vr::ETrackedPropertyError ret;
	state->pipe.recv(&ret, sizeof(ret));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathRead_t *it =  &pBatch[i];
		state->pipe.recv(&it->unTag, sizeof(it->unTag));
		state->pipe.recv(it->pvBuffer, it->unBufferSize);
		state->pipe.recv(&it->unRequiredBufferSize, sizeof(it->unRequiredBufferSize));
		state->pipe.recv(&it->eError, sizeof(it->eError));
	}

	state->pipe.return_read_channel();

	return ret;
}
MSABI vr::ETrackedPropertyError VRPaths::WritePathBatch(vr::PropertyContainerHandle_t ulRootHandle, vr::PathWrite_t * pBatch, uint32_t unBatchEntryCount) {
	state->pipe.msg("call WritePathBatch(%d, %p, %d)\n", ulRootHandle, pBatch, unBatchEntryCount);

	state->pipe.begin_call(METH_PATH_WRITE);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulRootHandle, sizeof(uint64_t));

	state->pipe.send(&unBatchEntryCount, sizeof(unBatchEntryCount));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathWrite_t *it =  &pBatch[i];
		state->pipe.send(&it->ulPath, sizeof(it->ulPath));
		state->pipe.send(&it->writeType, sizeof(it->writeType));
		state->pipe.send(&it->eSetError, sizeof(it->eSetError));
		state->pipe.send(&it->unBufferSize, sizeof(it->unBufferSize));
		state->pipe.send(it->pvBuffer, it->unBufferSize);
		if(it->pszPath != nullptr) {
			state->pipe.msg("The NULL alsoPath assumption did not hold %s", it->pszPath);
		}
	}

	state->pipe.wait_for_return();

	vr::ETrackedPropertyError ret;
	state->pipe.recv(&ret, sizeof(ret));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PathWrite_t *it =  &pBatch[i];
		state->pipe.recv(&it->unTag, sizeof(it->unTag));
		state->pipe.recv(&it->eError, sizeof(it->eError));
	}

	state->pipe.return_read_channel();

	return ret;
}
MSABI vr::ETrackedPropertyError VRPaths::StringToHandle(vr::PathHandle_t * pHandle, char * pchPath) {
	state->pipe.msg("call StringToHandle(%p, %s)\n", pHandle, pchPath);

	state->pipe.begin_call(METH_PATH_S2H);
	state->pipe.send(&objId, sizeof(uint64_t));
	uint64_t pathLen = strlen(pchPath);
	state->pipe.send(&pathLen, sizeof(uint64_t));
	state->pipe.send(pchPath, pathLen);

	state->pipe.wait_for_return();

	vr::ETrackedPropertyError ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(pHandle, sizeof(pHandle));
	state->pipe.return_read_channel();

	return ret;
}
MSABI vr::ETrackedPropertyError VRPaths::HandleToString(vr::PathHandle_t pHandle, char * pchBuffer, uint32_t unBufferSize, uint32_t * punBufferSizeUsed) {
	STUB();
	return vr::ETrackedPropertyError::TrackedProp_Success;
}

class VRServerDriverHost : public vr::IVRServerDriverHost {
	uint64_t objId;
	struct DriverState *state;

	public:
	VRServerDriverHost(struct DriverState *state, uint64_t objId);

	virtual MSABI bool TrackedDeviceAdded( const char *pchDeviceSerialNumber, vr::ETrackedDeviceClass eDeviceClass, vr::ITrackedDeviceServerDriver *pDriver );
	virtual MSABI void TrackedDevicePoseUpdated( uint32_t unWhichDevice, const vr::DriverPose_t & newPose, uint32_t unPoseStructSize );
	virtual MSABI void VsyncEvent( double vsyncTimeOffsetSeconds );
	virtual MSABI void VendorSpecificEvent( uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset );
	virtual MSABI bool IsExiting();
	virtual MSABI bool PollNextEvent( vr::VREvent_t *pEvent, uint32_t uncbVREvent );
	virtual MSABI void GetRawTrackedDevicePoses( float fPredictedSecondsFromNow, vr::TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount );
	virtual MSABI void RequestRestart( const char *pchLocalizedReason, const char *pchExecutableToStart, const char *pchArguments, const char *pchWorkingDirectory );
	virtual MSABI uint32_t GetFrameTimings( vr::Compositor_FrameTiming *pTiming, uint32_t nFrames );
	virtual MSABI void SetDisplayEyeToHead( uint32_t unWhichDevice, const vr::HmdMatrix34_t & eyeToHeadLeft, const vr::HmdMatrix34_t & eyeToHeadRight );
	virtual MSABI void SetDisplayProjectionRaw( uint32_t unWhichDevice, const vr::HmdRect2_t & eyeLeft, const vr::HmdRect2_t & eyeRight );
	virtual MSABI void SetRecommendedRenderTargetSize( uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight );
};

VRServerDriverHost::VRServerDriverHost(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {};

MSABI bool VRServerDriverHost::TrackedDeviceAdded(const char *pchDeviceSerialNumber, vr::ETrackedDeviceClass eDeviceClass, vr::ITrackedDeviceServerDriver *pDriver) {
	state->pipe.msg("call TrackedDeviceAdded(%s, %d, %p)\n", pchDeviceSerialNumber, eDeviceClass, pDriver);

	state->pipe.begin_call(METH_SERVER_DEVADD);
	state->pipe.send(&objId, sizeof(size_t));
	uint64_t len = strlen(pchDeviceSerialNumber);
	state->pipe.send(&len, sizeof(len));
	state->pipe.send(pchDeviceSerialNumber, len);
	state->pipe.send(&eDeviceClass, sizeof(vr::ETrackedDeviceClass));
	state->pipe.send_new_obj(pDriver);

	state->pipe.wait_for_return();

	uint8_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d\n", ret);
	return ret;
};
MSABI void VRServerDriverHost::TrackedDevicePoseUpdated(uint32_t unWhichDevice, const vr::DriverPose_t & newPose, uint32_t unPoseStructSize) {
	state->pipe.msg("call TrackedDevicePoseUpdated(%s, %p, %d)\n", unWhichDevice, newPose, unPoseStructSize);

	state->pipe.begin_call(METH_SERVER_POSE);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&unWhichDevice, sizeof(unWhichDevice));
	state->pipe.send(&newPose, sizeof(newPose));

	state->pipe.wait_for_return();
	state->pipe.return_read_channel();

	state->pipe.msg("ret\n");
}
MSABI void VRServerDriverHost::VsyncEvent(double vsyncTimeOffsetSeconds) {
	STUB();
}
MSABI void VRServerDriverHost::VendorSpecificEvent(uint32_t unWhichDevice, vr::EVREventType eventType, const vr::VREvent_Data_t & eventData, double eventTimeOffset) {
	STUB();
}
MSABI bool VRServerDriverHost::IsExiting() {
	STUB();
	return false;
}
MSABI bool VRServerDriverHost::PollNextEvent(vr::VREvent_t *pEvent, uint32_t uncbVREvent) {
	state->pipe.msg("call PollNextEvent(%p, %d)\n", pEvent, uncbVREvent);

	state->pipe.begin_call(METH_SERVER_POLL);
	state->pipe.send(&objId, sizeof(size_t));
	state->pipe.send(&uncbVREvent, sizeof(uncbVREvent));

	state->pipe.wait_for_return();

	uint8_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(pEvent, uncbVREvent);
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d\n", ret);
	return ret;
}
MSABI void VRServerDriverHost::GetRawTrackedDevicePoses(float fPredictedSecondsFromNow, vr::TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) {
	STUB();
}
MSABI void VRServerDriverHost::RequestRestart(const char *pchLocalizedReason, const char *pchExecutableToStart, const char *pchArguments, const char *pchWorkingDirectory) {
	STUB();
}
MSABI uint32_t VRServerDriverHost::GetFrameTimings(vr::Compositor_FrameTiming *pTiming, uint32_t nFrames) {
	STUB();
	return 0;
}
MSABI void VRServerDriverHost::SetDisplayEyeToHead(uint32_t unWhichDevice, const vr::HmdMatrix34_t & eyeToHeadLeft, const vr::HmdMatrix34_t & eyeToHeadRight) {
	STUB();
}
MSABI void VRServerDriverHost::SetDisplayProjectionRaw(uint32_t unWhichDevice, const vr::HmdRect2_t & eyeLeft, const vr::HmdRect2_t & eyeRight) {
	STUB();
}
MSABI void VRServerDriverHost::SetRecommendedRenderTargetSize(uint32_t unWhichDevice, uint32_t nWidth, uint32_t nHeight) {
	STUB();
}

class VRSettings : public vr::IVRSettings {
	uint64_t objId;
	struct DriverState *state;

	public:
	VRSettings(struct DriverState *state, uint64_t objId);

	virtual MSABI const char *GetSettingsErrorNameFromEnum( vr::EVRSettingsError eError );

	virtual MSABI void SetBool( const char *pchSection, const char *pchSettingsKey, bool bValue, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI void SetInt32( const char *pchSection, const char *pchSettingsKey, int32_t nValue, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI void SetFloat( const char *pchSection, const char *pchSettingsKey, float flValue, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI void SetString( const char *pchSection, const char *pchSettingsKey, const char *pchValue, vr::EVRSettingsError *peError = nullptr );

	virtual MSABI bool GetBool( const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI int32_t GetInt32( const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI float GetFloat( const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI void GetString( const char *pchSection, const char *pchSettingsKey, VR_OUT_STRING() char *pchValue, uint32_t unValueLen, vr::EVRSettingsError *peError = nullptr );

	virtual MSABI void RemoveSection( const char *pchSection, vr::EVRSettingsError *peError = nullptr );
	virtual MSABI void RemoveKeyInSection( const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError = nullptr );
};

VRSettings::VRSettings(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {};

MSABI const char *VRSettings::GetSettingsErrorNameFromEnum(vr::EVRSettingsError eError) {
	STUB();
	return "";
}

MSABI void VRSettings::SetBool(const char *pchSection, const char *pchSettingsKey, bool bValue, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}
MSABI void VRSettings::SetInt32(const char *pchSection, const char *pchSettingsKey, int32_t nValue, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}
MSABI void VRSettings::SetFloat(const char *pchSection, const char *pchSettingsKey, float flValue, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}
MSABI void VRSettings::SetString(const char *pchSection, const char *pchSettingsKey, const char *pchValue, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}

MSABI bool VRSettings::GetBool(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError) {
	state->pipe.msg("call GetBool(%s, %s, %p)\n", pchSection, pchSettingsKey, peError);

	if(peError == nullptr) {
		vr::EVRSettingsError err;
		peError = &err;
	}

	state->pipe.begin_call(METH_SETS_GBOOL);
	state->pipe.send(&objId, sizeof(objId));
	uint64_t sectionLen = strlen(pchSection);
	state->pipe.send(&sectionLen, sizeof(sectionLen));
	state->pipe.send(pchSection, sectionLen);
	uint64_t keyLen = strlen(pchSettingsKey);
	state->pipe.send(&keyLen, sizeof(keyLen));
	state->pipe.send(pchSettingsKey, keyLen);

	state->pipe.wait_for_return();

	uint8_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(peError, sizeof(*peError));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d %d\n", ret, *peError);
	return ret;
}
MSABI int32_t VRSettings::GetInt32(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError) {
	state->pipe.msg("call GetInt32(%s, %s, %p)\n", pchSection, pchSettingsKey, peError);

	if(peError == nullptr) {
		vr::EVRSettingsError err;
		peError = &err;
	}

	state->pipe.begin_call(METH_SETS_GINT);
	state->pipe.send(&objId, sizeof(objId));
	uint64_t sectionLen = strlen(pchSection);
	state->pipe.send(&sectionLen, sizeof(sectionLen));
	state->pipe.send(pchSection, sectionLen);
	uint64_t keyLen = strlen(pchSettingsKey);
	state->pipe.send(&keyLen, sizeof(keyLen));
	state->pipe.send(pchSettingsKey, keyLen);

	state->pipe.wait_for_return();

	int32_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(peError, sizeof(*peError));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %d %d\n", ret, *peError);
	return ret;
}
MSABI float VRSettings::GetFloat(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
	return 0.0;
}
MSABI void VRSettings::GetString(const char *pchSection, const char *pchSettingsKey, VR_OUT_STRING() char *pchValue, uint32_t unValueLen, vr::EVRSettingsError *peError) {
	state->pipe.msg("call GetString(%s, %s, %p, %d, %p)\n", pchSection, pchSettingsKey, pchValue, unValueLen, peError);

	if(peError == nullptr) {
		vr::EVRSettingsError err;
		peError = &err;
	}

	state->pipe.begin_call(METH_SETS_GSTR);
	state->pipe.send(&objId, sizeof(objId));
	uint64_t sectionLen = strlen(pchSection);
	state->pipe.send(&sectionLen, sizeof(sectionLen));
	state->pipe.send(pchSection, sectionLen);
	uint64_t keyLen = strlen(pchSettingsKey);
	state->pipe.send(&keyLen, sizeof(keyLen));
	state->pipe.send(pchSettingsKey, keyLen);
	state->pipe.send(&unValueLen, sizeof(unValueLen));

	state->pipe.wait_for_return();

	state->pipe.recv(pchValue, unValueLen);
	state->pipe.recv(peError, sizeof(*peError));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %s\n", pchValue);
	return;
}

MSABI void VRSettings::RemoveSection(const char *pchSection, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}
MSABI void VRSettings::RemoveKeyInSection(const char *pchSection, const char *pchSettingsKey, vr::EVRSettingsError *peError) {
	STUB();
	if(peError != nullptr) *peError = vr::EVRSettingsError::VRSettingsError_IPCFailed;
}

class VRProperties : public vr::IVRProperties {
	struct DriverState *state;
	uint64_t objId;

	public:
	VRProperties(struct DriverState *state, uint64_t objId);

	virtual MSABI vr::ETrackedPropertyError ReadPropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle, vr::PropertyRead_t *pBatch, uint32_t unBatchEntryCount);
	virtual MSABI vr::ETrackedPropertyError WritePropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle, vr::PropertyWrite_t *pBatch, uint32_t unBatchEntryCount);
	virtual MSABI const char *GetPropErrorNameFromEnum(vr::ETrackedPropertyError error);
	virtual MSABI vr::PropertyContainerHandle_t TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t nDevice);
};

VRProperties::VRProperties(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {};

MSABI vr::ETrackedPropertyError VRProperties::ReadPropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle, vr::PropertyRead_t *pBatch, uint32_t unBatchEntryCount) {
	state->pipe.msg("call ReadPropertyBatch(%d, %p, %d)\n", ulContainerHandle, pBatch, unBatchEntryCount);

	state->pipe.begin_call(METH_PROP_READ);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulContainerHandle, sizeof(uint64_t));

	state->pipe.send(&unBatchEntryCount, sizeof(unBatchEntryCount));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PropertyRead_t *it =  &pBatch[i];
		state->pipe.send(&it->prop, sizeof(it->prop));
		state->pipe.send(&it->unBufferSize, sizeof(it->unBufferSize));
		// I don't quite know which direction this goes in
		state->pipe.send(&it->unTag, sizeof(it->unTag));
	}

	state->pipe.wait_for_return();

	vr::ETrackedPropertyError ret;
	state->pipe.recv(&ret, sizeof(ret));

	state->pipe.msg("ret %d\n", ret);
	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PropertyRead_t *it =  &pBatch[i];
		state->pipe.recv(&it->unTag, sizeof(it->unTag));
		state->pipe.recv(it->pvBuffer, it->unBufferSize);
		state->pipe.recv(&it->unRequiredBufferSize, sizeof(it->unRequiredBufferSize));
		state->pipe.recv(&it->eError, sizeof(it->eError));
		state->pipe.msg("    {%d %.*s %d %d}\n", it->unTag, it->unBufferSize, it->pvBuffer, it->unRequiredBufferSize, it->eError);
	}

	state->pipe.return_read_channel();

	return ret;
}
MSABI vr::ETrackedPropertyError VRProperties::WritePropertyBatch(vr::PropertyContainerHandle_t ulContainerHandle, vr::PropertyWrite_t *pBatch, uint32_t unBatchEntryCount) {
	state->pipe.msg("call WritePropertyBatch(%d, %p, %d)\n", ulContainerHandle, pBatch, unBatchEntryCount);

	state->pipe.begin_call(METH_PROP_WRITE);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&ulContainerHandle, sizeof(uint64_t));

	state->pipe.send(&unBatchEntryCount, sizeof(unBatchEntryCount));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PropertyWrite_t *it =  &pBatch[i];
		state->pipe.send(&it->prop, sizeof(it->prop));
		state->pipe.send(&it->writeType, sizeof(it->writeType));
		state->pipe.send(&it->eSetError, sizeof(it->eSetError));
		state->pipe.send(&it->unBufferSize, sizeof(it->unBufferSize));
		state->pipe.send(it->pvBuffer, it->unBufferSize);
		state->pipe.send(&it->unTag, sizeof(it->unTag));
	}

	state->pipe.wait_for_return();

	vr::ETrackedPropertyError ret;
	state->pipe.recv(&ret, sizeof(ret));

	for(uint64_t i = 0; i < unBatchEntryCount; i++) {
		vr::PropertyWrite_t *it =  &pBatch[i];
		state->pipe.recv(&it->unTag, sizeof(it->unTag));
		state->pipe.recv(&it->eError, sizeof(it->eError));
	}

	state->pipe.return_read_channel();

	return ret;
}
MSABI const char *VRProperties::GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) {
	STUB();
	return "";
}
MSABI vr::PropertyContainerHandle_t VRProperties::TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t nDevice) {
	state->pipe.msg("call TrackedDeviceToPropertyContainer(%d)\n", nDevice);

	state->pipe.begin_call(METH_PROP_TRANS);
	state->pipe.send(&objId, sizeof(objId));
	state->pipe.send(&nDevice, sizeof(nDevice));

	state->pipe.wait_for_return();

	vr::PropertyContainerHandle_t ret;
	state->pipe.recv(&ret, sizeof(ret));

	state->pipe.return_read_channel();

	return ret;
}

class VRDriverLog : public vr::IVRDriverLog {
	struct DriverState *state;
	uint64_t objId;

	public:
	VRDriverLog(struct DriverState *state, uint64_t objId);

	virtual MSABI void Log(const char *pchLogMessage);
};

VRDriverLog::VRDriverLog(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {};

MSABI void VRDriverLog::Log(const char *pchLogMessage) {
	std::cerr << "Log: " << pchLogMessage << std::endl;
	STUB();
}

class VRDriverManager : public vr::IVRDriverManager {
	struct DriverState *state;
	uint64_t objId;

	public:
	VRDriverManager(struct DriverState *state, uint64_t objId);

	virtual MSABI uint32_t GetDriverCount() const;
	virtual MSABI uint32_t GetDriverName(vr::DriverId_t nDriver, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize);
	virtual MSABI vr::DriverHandle_t GetDriverHandle(const char *pchDriverName);
	virtual MSABI bool IsEnabled(vr::DriverId_t nDriver) const;
};

VRDriverManager::VRDriverManager(struct DriverState *state, uint64_t objId) : state(state), objId(objId) { }

uint32_t VRDriverManager::GetDriverCount() const {
	STUB();
	return 0;
}
uint32_t VRDriverManager::GetDriverName(vr::DriverId_t nDriver, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize) {
	STUB();
	return 0;
}
vr::DriverHandle_t VRDriverManager::GetDriverHandle(const char *pchDriverName) {
	STUB();
	return 0;
}
bool VRDriverManager::IsEnabled(vr::DriverId_t nDriver) const {
	STUB();
	return true;
}

class VRResources : public vr::IVRResources {
	struct DriverState *state;
	uint64_t objId;

	public:
	VRResources(struct DriverState *state, uint64_t objId);

	virtual MSABI uint32_t LoadSharedResource(const char *pchResourceName, char *pchBuffer, uint32_t unBufferLen);
	virtual MSABI uint32_t GetResourceFullPath(const char *pchResourceName, const char *pchResourceTypeDirectory, char *pchPathBuffer, uint32_t unBufferLen);
};

VRResources::VRResources(struct DriverState *state, uint64_t objId) : state(state), objId(objId) { }

MSABI uint32_t VRResources::LoadSharedResource(const char *pchResourceName, char *pchBuffer, uint32_t unBufferLen) {
	state->pipe.msg("call LoadSharedResource(%s, %p, %d)\n", pchResourceName, pchBuffer, unBufferLen);

	state->pipe.begin_call(METH_RES_LOAD);
	state->pipe.send(&objId, sizeof(size_t));
	uint64_t len = strlen(pchResourceName);
	state->pipe.send(&len, sizeof(uint64_t));
	state->pipe.send(pchResourceName, len);
	state->pipe.send(&unBufferLen, sizeof(uint32_t));

	state->pipe.wait_for_return();

	uint32_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(pchBuffer, min(ret, unBufferLen));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %.*s, %d\n", min(ret, unBufferLen), pchBuffer, ret);
	return ret;
}
MSABI uint32_t VRResources::GetResourceFullPath(const char *pchResourceName, const char *pchResourceTypeDirectory, char *pchPathBuffer, uint32_t unBufferLen) {
	state->pipe.msg("call GetResourceFullPath(%s, %s, %p, %d)\n", pchResourceName, pchResourceTypeDirectory, pchPathBuffer, unBufferLen);

	state->pipe.begin_call(METH_RES_PATH);
	state->pipe.send(&this->objId, sizeof(uint64_t));
	uint64_t nameLen = strlen(pchResourceName);
	state->pipe.send(&nameLen, sizeof(uint64_t));
	state->pipe.send(pchResourceName, nameLen);
	if(pchResourceTypeDirectory == nullptr) {
		uint64_t zero = 0;
		state->pipe.send(&zero, sizeof(uint64_t));
	} else {
		uint64_t dirLen = strlen(pchResourceTypeDirectory);
		assert(dirLen > 0);
		state->pipe.send(&dirLen, sizeof(uint64_t));
		state->pipe.send(pchResourceTypeDirectory, dirLen);
	}
	state->pipe.send(&unBufferLen, sizeof(uint32_t));

	state->pipe.wait_for_return();

	uint32_t ret;
	state->pipe.recv(&ret, sizeof(ret));
	state->pipe.recv(pchPathBuffer, min(ret, unBufferLen));
	state->pipe.return_read_channel();

	state->pipe.msg("ret %.*s, %d\n", min(ret, unBufferLen), pchPathBuffer, ret);
	return ret;
}

class VRServerConnector : public vr::IVRDriverContext {
	uint64_t objId;
	struct DriverState *state;

	public:
	VRServerConnector(struct DriverState *state, uint64_t objId);

	virtual MSABI void *GetGenericInterface( const char *pchInterfaceVersion, vr::EVRInitError *peError = nullptr ) override;
	virtual MSABI vr::DriverHandle_t GetDriverHandle() override;
};

VRServerConnector::VRServerConnector(struct DriverState *state, uint64_t objId) : state(state), objId(objId) {
};

MSABI void *VRServerConnector::GetGenericInterface( const char *pchInterfaceVersion, vr::EVRInitError *peError) {
	std::cout << "Load interface: " << pchInterfaceVersion << ", arg2: " << (void*) peError << std::endl;

	state->pipe.begin_call(METH_GET_INTERFACE);
	state->pipe.send(&objId, sizeof(size_t));
	uint64_t len = strlen(pchInterfaceVersion);
	state->pipe.send(&len, sizeof(uint64_t));
	state->pipe.send(pchInterfaceVersion, len);

	state->pipe.wait_for_return();

	uint64_t objId;
	state->pipe.recv(&objId, sizeof(uint64_t));
	{
		vr::EVRInitError err;
		state->pipe.recv(&err, sizeof(vr::EVRInitError));
		if(peError != nullptr) {
			*peError = err;
		}
	}

	state->pipe.return_read_channel();

	if(objId == 0) {
		if(peError != nullptr) {
			*peError =  vr::EVRInitError::VRInitError_Init_InterfaceNotFound;
		}
		return nullptr;
	}

	if(peError != nullptr) {
		*peError =  vr::EVRInitError::VRInitError_None;
	}
	if(strcmp(pchInterfaceVersion, vr::IVRServerDriverHost_Version) == 0) {
		std::cout << "Create and return a VRServerDriverHost" << std::endl;
		return new VRServerDriverHost(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRSettings_Version) == 0) {
		std::cout << "Create and return a VRSettings" << std::endl;
		return new VRSettings(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRProperties_Version) == 0) {
		std::cout << "Create and return a VRProperties" << std::endl;
		return new VRProperties(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRDriverLog_Version) == 0) {
		std::cout << "Create and return a VRDriverLog" << std::endl;
		return new VRDriverLog(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRDriverManager_Version) == 0) {
		std::cout << "Create and return a VRDriverManager" << std::endl;
		return new VRDriverManager(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRResources_Version) == 0) {
		std::cout << "Create and return a VRResources" << std::endl;
		return new VRResources(state, objId);
	} else if(strcmp(pchInterfaceVersion, vr::IVRDriverInput_Version) == 0) {
		std::cout << "Create and return a VRDriverInput" << std::endl;
		return new VRDriverInput(state, objId);
	/* } else if(strcmp(pchInterfaceVersion, "IVRRenderModels_006") == 0) { */
	/* 	std::cout << "Create and return a VRRenderModels" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRRenderModelsInternal_XXX") == 0) { */
	/* 	std::cout << "Create and return a VRRenderModelsInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRSettingsInternal_001") == 0) { */
	/* 	std::cout << "Create and return a VRSettingsInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	} else if(strcmp(pchInterfaceVersion, "IVRPaths_001") == 0) {
		std::cout << "Create and return a VRPaths" << std::endl;
		return new VRPaths(state, objId);
	/* } else if(strcmp(pchInterfaceVersion, "IVRPathsInternal_001") == 0) { */
	/* 	std::cout << "Create and return a VRPathsInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRPropertiesInternal_001") == 0) { */
	/* 	std::cout << "Create and return a VRPropertiesInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	} else if(strcmp(pchInterfaceVersion, "IVRServer_XXX") == 0) {
		// This looks like it's used for some telemetry stuff.
		// I can't find the interface anywhere, but returning null looks to disable it
		return nullptr;
	/* } else if(strcmp(pchInterfaceVersion, "IVRSystemLayerInternal") == 0) { */
	/* 	std::cout << "Create and return a VRSystemLayerInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRClientInternal_XXX") == 0) { */
	/* 	std::cout << "Create and return a VRClientInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRCompositorSystemInternal_001") == 0) { */
	/* 	std::cout << "Create and return a VRCompositorSystemInternal" << std::endl; */
	/* 	return new VRResources(state, objId); */
	/* } else if(strcmp(pchInterfaceVersion, "IVRInput_010") == 0) { */
	/* 	std::cout << "Create and return a VRInput" << std::endl; */
	/* 	return new VRResources(state, objId); */
	} else if(strcmp(pchInterfaceVersion, vr::IVRMailbox_Version) == 0) {
		std::cout << "Create and return a VRMailbox" << std::endl;
		return new VRMailbox(state, objId);
	}
	
	if(peError != nullptr) {
		/* *peError =  vr::EVRInitError::VRInitError_Init_InterfaceNotFound; */
		*peError =  vr::EVRInitError::VRInitError_None;
	}
	// Instead of returning and error, we hide behind returning a pointer
	// that will crash if any of the virtual methods are ever accessed
	std::cerr << "Returning a null area pointer" << std::endl;
	return new NullObject {
		.vtable = &nullspot,
		.name = strdup(pchInterfaceVersion),
	};
	/* return nullptr; */
}
vr::DriverHandle_t VRServerConnector::GetDriverHandle() {
	STUB();
	return 0x42424242;
}

static void handler(enum PipeMethod m, void *state_) {
	struct DriverState *state = (struct DriverState*)state_;
	switch(m) {
	case METH_DRIVER_FACTORY: {
		size_t nameLen;
		state->pipe.recv(&nameLen, sizeof(size_t));
		state->pipe.msg("Arg is %ld long\n", nameLen);
		char *buf = (char*)malloc(nameLen + 1);
		state->pipe.recv(buf, nameLen);
		size_t taskId = state->pipe.complete_reading_args();

		buf[nameLen] = '\0';
		std::cerr << "buffer " << nameLen << " content: " << buf << "\n";

		HmdDriverFactoryPtr driverFactory = (HmdDriverFactoryPtr)GetProcAddress(state->hDLL, "HmdDriverFactory");
		if(driverFactory == NULL) {
			state->pipe.msg("Driver factory failed to load");
		}
		int rc = 0;
		vr::IServerTrackedDeviceProvider* driver = (vr::IServerTrackedDeviceProvider*)driverFactory(buf, &rc);
			std::cout << "Return Code: " << rc << std::endl;

		uint64_t nextId = 0;
		if(driver != nullptr) {
			state->pipe.objs.push_back(driver);
			nextId = state->pipe.objs.size();
		}

		state->pipe.return_from_call(taskId);
		state->pipe.send(&nextId, sizeof(uint64_t));
		state->pipe.send(&rc, sizeof(int));
		break;
	}
	case METH_DRIVER_INIT: {
		size_t objId;
		state->pipe.recv(&objId, sizeof(size_t));
		assert(objId != 0);
		vr::IServerTrackedDeviceProvider *driver = (vr::IServerTrackedDeviceProvider*)state->pipe.objs[objId-1];

		uint64_t contextObjId;
		state->pipe.recv(&contextObjId, sizeof(size_t));
		size_t taskId = state->pipe.complete_reading_args();

		VRServerConnector *connector = new VRServerConnector(state, contextObjId);
		vr::EVRInitError err = driver->Init(connector);
		if(err != vr::EVRInitError::VRInitError_None) {
			std::cerr << "Driver Init Error: " << err << std::endl;
		}
		state->pipe.return_from_call(taskId);
		state->pipe.send(&err, sizeof(vr::EVRInitError));
		break;
	}
	case METH_DEV_ACTIVATE: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::ITrackedDeviceServerDriver *thisObj = (vr::ITrackedDeviceServerDriver*)state->pipe.objs[thisHandle-1];

		uint32_t objectId;
		state->pipe.recv(&objectId, sizeof(objectId));
		size_t taskId = state->pipe.complete_reading_args();

		vr::EVRInitError ret = thisObj->Activate(objectId);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_DEV_COMPONENT: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(thisHandle));
		assert(thisHandle != 0);
		vr::ITrackedDeviceServerDriver *thisObj = (vr::ITrackedDeviceServerDriver*)state->pipe.objs[thisHandle-1];

		uint64_t nameLen;
		state->pipe.recv(&nameLen, sizeof(nameLen));
		char *name = (char*)malloc(nameLen + 1);
		state->pipe.recv(name, nameLen);
		name[nameLen] = '\0';
		size_t taskId = state->pipe.complete_reading_args();

		state->pipe.msg("Calling getcomponent with %s\n", name);
		void *ret = thisObj->GetComponent(name);
		state->pipe.msg("Ret from getcomponent %p\n", ret);

		state->pipe.return_from_call(taskId);
		state->pipe.send_new_obj(ret);
		break;
	}
	case METH_COMP_WINSIZE: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		int32_t x;
		int32_t y;
		uint32_t width;
		uint32_t height;
		thisObj->GetWindowBounds(&x, &y, &width, &height);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&x, sizeof(x));
		state->pipe.send(&y, sizeof(y));
		state->pipe.send(&width, sizeof(width));
		state->pipe.send(&height, sizeof(height));
		break;
	}
	case METH_COMP_DISTORTION: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		vr::EVREye eye;
		state->pipe.recv(&eye, sizeof(eye));
		float fU;
		state->pipe.recv(&fU, sizeof(fU));
		float fV;
		state->pipe.recv(&fV, sizeof(fV));
		size_t taskId = state->pipe.complete_reading_args();

		vr::DistortionCoordinates_t ret;
		thisObj->ComputeDistortion(&ret, eye, fU, fV);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_COMP_EYEVIEWPORT: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		vr::EVREye eye;
		state->pipe.recv(&eye, sizeof(eye));
		size_t taskId = state->pipe.complete_reading_args();

		uint32_t x;
		uint32_t y;
		uint32_t width;
		uint32_t height;
		thisObj->GetEyeOutputViewport(eye, &x, &y, &width, &height);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&x, sizeof(x));
		state->pipe.send(&y, sizeof(y));
		state->pipe.send(&width, sizeof(width));
		state->pipe.send(&height, sizeof(height));
		break;
	}
	case METH_COMP_ONDESKTOP: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		bool ret = thisObj->IsDisplayOnDesktop();

		state->pipe.return_from_call(taskId);
		state->pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_COMP_REALDISPLAY: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		bool ret = thisObj->IsDisplayRealDisplay();

		state->pipe.return_from_call(taskId);
		state->pipe.send(&ret, sizeof(ret));
		break;
	}
	case METH_COMP_PROJRAW: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		vr::EVREye eye;
		state->pipe.recv(&eye, sizeof(eye));
		size_t taskId = state->pipe.complete_reading_args();

		float left;
		float right;
		float top;
		float bottom;
		thisObj->GetProjectionRaw(eye, &left, &right, &top, &bottom);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&left, sizeof(left));
		state->pipe.send(&right, sizeof(right));
		state->pipe.send(&top, sizeof(top));
		state->pipe.send(&bottom, sizeof(bottom));
		break;
	}
	case METH_COMP_TARGETSIZE: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(size_t));
		assert(thisHandle != 0);
		vr::IVRDisplayComponent *thisObj = (vr::IVRDisplayComponent*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		uint32_t width;
		uint32_t height;
		thisObj->GetRecommendedRenderTargetSize(&width, &height);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&width, sizeof(width));
		state->pipe.send(&height, sizeof(height));
		break;
	}
	case METH_DIRECT_CSWAP: {
		state->pipe.msg("CSWAP\n");
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		uint32_t pid;
		state->pipe.recv(&pid, sizeof(pid));
		vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t textureDesc;
		state->pipe.recv(&textureDesc, sizeof(textureDesc));
		state->pipe.msg("%d %d %d %d\n", textureDesc.nWidth, textureDesc.nHeight, textureDesc.nFormat, textureDesc.nSampleCount);

		size_t taskId = state->pipe.complete_reading_args();

		vr::IVRDriverDirectModeComponent::SwapTextureSet_t texture;
		thisObj->CreateSwapTextureSet(pid, &textureDesc, &texture);

		state->pipe.msg("Converting HANDLEs to fds\n");
		int linuxHandles[3];
		for(uint8_t i = 0; i < 3; i++) {
			HANDLE handle = (HANDLE)texture.rSharedTextureHandles[i];

			int fd;
			fd = get_shared_resource_fd(handle, state);
			state->pipe.msg("    0x%08X -> 0x%08X\n", handle, fd);
			/* wine_server_handle_to_fd(handle, FILE_READ_DATA, &fd, NULL); */
			linuxHandles[i] = fd;
		}

		state->pipe.return_from_call(taskId);
		state->pipe.send(&texture.unTextureFlags, sizeof(texture.unTextureFlags));
		state->pipe.msg("Sending fds\n");
		state->pipe.send_fd(linuxHandles[0]);
		state->pipe.send_fd(linuxHandles[1]);
		state->pipe.send_fd(linuxHandles[2]);
		// Send some numbers that it can use to refer to the textures
		// By using the handles we'd have to use we avoid having to translate anything
		state->pipe.send(&texture.rSharedTextureHandles[0], sizeof(texture.rSharedTextureHandles[0]));
		state->pipe.send(&texture.rSharedTextureHandles[1], sizeof(texture.rSharedTextureHandles[1]));
		state->pipe.send(&texture.rSharedTextureHandles[2], sizeof(texture.rSharedTextureHandles[2]));
		break;
	}
	case METH_DIRECT_NEXT: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		vr::SharedTextureHandle_t tex[2];
		state->pipe.recv(&tex[0], sizeof(tex[0]));
		state->pipe.recv(&tex[1], sizeof(tex[1]));

		size_t taskId = state->pipe.complete_reading_args();

		uint32_t indecies[2];
		thisObj->GetNextSwapTextureSetIndex(tex, &indecies);

		state->pipe.return_from_call(taskId);

		state->pipe.send(indecies, sizeof(indecies));
		break;
	}
	case METH_DIRECT_SUBMIT: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t perEye[2];
		for(uint8_t i = 0; i < 2; i++) {
			state->pipe.recv(&perEye[i].hTexture, sizeof(perEye[i].hTexture));
			state->pipe.recv(&perEye[i].hDepthTexture, sizeof(perEye[i].hDepthTexture));
			state->pipe.recv(&perEye[i].bounds, sizeof(perEye[i].bounds));
			state->pipe.recv(&perEye[i].mProjection, sizeof(perEye[i].mProjection));
			state->pipe.recv(&perEye[i].mHmdPose, sizeof(perEye[i].mHmdPose));
			state->pipe.recv(&perEye[i].flHmdPosePredictionTimeInSecondsFromNow, sizeof(perEye[i].flHmdPosePredictionTimeInSecondsFromNow));
		}

		size_t taskId = state->pipe.complete_reading_args();

		vr::IVRDriverDirectModeComponent::SwapTextureSet_t texture;
		thisObj->SubmitLayer(perEye);

		state->pipe.return_from_call(taskId);
		break;
	}
	case METH_DIRECT_PRESENT: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		vr::SharedTextureHandle_t tex;
		state->pipe.recv(&tex, sizeof(tex));

		size_t taskId = state->pipe.complete_reading_args();

		thisObj->Present(tex);

		state->pipe.return_from_call(taskId);
		break;
	}
	case METH_DIRECT_POSTPRES: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		vr::IVRDriverDirectModeComponent::Throttling_t throttle;
		state->pipe.recv(&throttle, sizeof(throttle));

		size_t taskId = state->pipe.complete_reading_args();

		thisObj->PostPresent(&throttle);

		state->pipe.return_from_call(taskId);
		break;
	}
	case METH_DIRECT_FTIME: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IVRDriverDirectModeComponent *thisObj = (vr::IVRDriverDirectModeComponent*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		vr::DriverDirectMode_FrameTiming frameTime;
		thisObj->GetFrameTiming(&frameTime);

		state->pipe.return_from_call(taskId);
		state->pipe.send(&frameTime, sizeof(frameTime));
		break;
	}
	case METH_DRIVER_RUNFRAME: {
		size_t thisHandle;
		state->pipe.recv(&thisHandle, sizeof(uint64_t));
		assert(thisHandle != 0);
		vr::IServerTrackedDeviceProvider *thisObj = (vr::IServerTrackedDeviceProvider*)state->pipe.objs[thisHandle-1];

		size_t taskId = state->pipe.complete_reading_args();

		thisObj->RunFrame();
		state->pipe.msg("Frames done!\n");

		state->pipe.return_from_call(taskId);
		break;
	}
	default:
		state->pipe.msg("Unhandled Method: %s\n", m);
		abort();
	}
}

void * TrampolineHook(char* src, char* dst, int len) {
    if (len < 5) return 0;

    void* gateway = VirtualAlloc(0, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(gateway, src, len);

    uintptr_t  gatewayRelativeAddr = ((uintptr_t)src - (uintptr_t)gateway) - 5;
    *(char*)((uintptr_t)gateway + len) = 0xE9;
    *(uintptr_t *)((uintptr_t)gateway + len + 1) = gatewayRelativeAddr;

    DWORD  curProtection;
    VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &curProtection);
    uintptr_t  relativeAddress = ((uintptr_t)dst - (uintptr_t)src) - 5;
    *src = 0xE9;
    *(uintptr_t *)((uintptr_t)src + 1) = relativeAddress;
    VirtualProtect(src, len, curProtection, &curProtection);

    return gateway;
}

extern "C" int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline, int cmdshow) {
	const char* driver_path = "/home/delusional/.local/share/Steam/ubuntu12_32/steamapps/content/app_17770/depot_250821/drivers/vrlink/bin/win64/driver_vrlink.dll";
	std::cerr << "Loading real vrlink driver: " << driver_path << std::endl;

	HMODULE hDLL = LoadLibraryA(driver_path);

	struct DriverState state_{
		.pipe = Pipe(true, handler),
		.hDLL = hDLL,
	};
	struct DriverState *state = &state_;
	DIR *dir = opendir("/tmp/");

	if (dir == NULL) {
		perror("opendir");
		return 1;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		std::cout << entry->d_name << "\n";
	}

	closedir(dir);

	if(state->hDLL == NULL) {
		std::cerr << "DLL Load Failed: " << GetLastError() << std::endl;
		return 1;
	}

	state->pipe.dispatch_requests(state);

	FreeLibrary(state->hDLL);
	return 0;
}
