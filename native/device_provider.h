#pragma once
#include <memory>

#include "controller_device.h"
#include "hmd_device_driver.h"
#include "openvr_driver.h"

class DeviceProvider : public vr::IServerTrackedDeviceProvider {
public:
	uint64_t handle;

    // Inherited via IServerTrackedDeviceProvider
    vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override;
    void Cleanup() override;
    const char* const* GetInterfaceVersions() override;
    void RunFrame() override;
    bool ShouldBlockStandbyMode() override;
    void EnterStandby() override;
    void LeaveStandby() override;

private:
    std::unique_ptr<ControllerDevice> my_left_device_;
    std::unique_ptr<ControllerDevice> my_right_device_;
    std::unique_ptr<MyHMDControllerDeviceDriver> hmd_device_;
};
