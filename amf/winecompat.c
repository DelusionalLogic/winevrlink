#include "winecompat.h"

// This include block is bonkers. If you touch it, it'll break
#include "lib/wine/include/ntstatus.h"
#define WIN32_NO_STATUS
#include "lib/wine/include/windef.h"
#define __WINESRC__ 1
#include "lib/wine/include/aclui.h"
#undef __WINESRC__
#include "lib/wine/include/winuser.h"
#include "lib/wine/include/winbase.h"
#include "lib/wine/include/winternl.h"
#include "lib/wine/include/ntuser.h"
#define WINE_UNIX_LIB 1
#include "lib/wine/include/wine/vulkan_driver.h"
#undef WINE_UNIX_LIB
#include "lib/wine/dlls/winevulkan/vulkan_private.h"

void* wine_unwrap_instance(void* handle) {
	assert(handle != NULL);
	return vulkan_instance_from_handle(handle)->host.instance;
}

void* wine_unwrap_phys_dev(void* handle) {
	assert(handle != NULL);
	return vulkan_physical_device_from_handle(handle)->host.physical_device;
}

void* wine_unwrap_device(void* handle) {
	assert(handle != NULL);
	return vulkan_device_from_handle(handle)->host.device;
}

void* wine_unwrap_dev_mem(void* handle) {
	assert(handle != NULL);
	return (void*)wine_device_memory_from_handle((VkDeviceMemory)handle)->host.device_memory;
}

void* wine_unwrap_queue(void* handle) {
	assert(handle != NULL);
	return vulkan_queue_from_handle(handle)->host.queue;
}
