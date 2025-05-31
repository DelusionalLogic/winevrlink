#pragma once

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
};
