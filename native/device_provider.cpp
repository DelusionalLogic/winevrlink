#include "device_provider.h"
#include "ipc.h"

extern Pipe global_pipe;

vr::EVRInitError DeviceProvider::Init(vr::IVRDriverContext* pDriverContext) {
	VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
	vr::VRDriverLog()->Log("Hello world!");

	// my_left_device_ = std::make_unique<ControllerDevice>(vr::TrackedControllerRole_LeftHand);
	// if(!vr::VRServerDriverHost()->TrackedDeviceAdded("LEFTHAND_ABC123",
	// 	vr::TrackedDeviceClass_Controller,
	// 	my_left_device_.get())) {
	// 	return vr::VRInitError_Driver_Unknown;
	// }

	// my_right_device_ = std::make_unique<ControllerDevice>(vr::TrackedControllerRole_RightHand);
	// if(!vr::VRServerDriverHost()->TrackedDeviceAdded("RIGHTHAND_ABC123",
	// 	vr::TrackedDeviceClass_Controller,
	// 	my_right_device_.get())) {
	// 	return vr::VRInitError_Driver_Unknown;
	// }

	/* hmd_device_ = std::make_unique<MyHMDControllerDeviceDriver>(); */
	/* if(!vr::VRServerDriverHost()->TrackedDeviceAdded("mycoolhmd", */
	/* 	vr::TrackedDeviceClass_HMD, */
	/* 	hmd_device_.get())) { */
	/* 	return vr::VRInitError_Driver_Unknown; */
	/* } */

	enum PipeMethod method = METH_DRIVER_INIT;

	global_pipe.msg("Sending init call %d\n", method);

	global_pipe.begin_call(method);

	global_pipe.msg("Sent init call %d\n", method);

	global_pipe.send(&this->handle, sizeof(size_t));

	global_pipe.send_new_obj(pDriverContext);

	global_pipe.msg("Waiting for interface lookups\n");

	global_pipe.wait_for_return();
	global_pipe.msg("Interface lookups done\n");

	vr::EVRInitError err;
	global_pipe.recv(&err, sizeof(vr::EVRInitError));
	global_pipe.return_read_channel();

	return err;
}

void DeviceProvider::Cleanup() {
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

const char* const* DeviceProvider::GetInterfaceVersions() {
    return vr::k_InterfaceVersions;
}

void DeviceProvider::RunFrame() {
	global_pipe.msg("Call RunFrame()\n");
    vr::VREvent_t vrevent;
    while (vr::VRServerDriverHost()->PollNextEvent(&vrevent, sizeof(vrevent))) {
        // my_left_device_->HandleEvent(vrevent);
        // my_right_device_->HandleEvent(vrevent);
		// hmd_device_->MyProcessEvent(vrevent);
    }

    if (my_left_device_ != nullptr) {
        my_left_device_->RunFrame();
    }

    if (my_right_device_ != nullptr) {
        my_right_device_->RunFrame();
    }

    if (hmd_device_ != nullptr) {
        hmd_device_->MyRunFrame();
    }

	global_pipe.begin_call(METH_DRIVER_RUNFRAME);
	global_pipe.send(&this->handle, sizeof(uint64_t));
	global_pipe.wait_for_return();
	global_pipe.return_read_channel();
}

bool DeviceProvider::ShouldBlockStandbyMode() {
    return false;
}

void DeviceProvider::EnterStandby() {
}

void DeviceProvider::LeaveStandby() {
}
