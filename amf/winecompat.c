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
#include "lib/wine/dlls/winevulkan/vulkan_private.h"

void* wine_unwrap_instance(void* handle) {
	assert(handle != NULL);
	return wine_instance_from_handle(handle)->host_instance;
}

void* wine_unwrap_phys_dev(void* handle) {
	assert(handle != NULL);
	return wine_phys_dev_from_handle(handle)->host_physical_device;
}

void* wine_unwrap_device(void* handle) {
	assert(handle != NULL);
	return wine_device_from_handle(handle)->host_device;
}

void* wine_unwrap_dev_mem(void* handle) {
	assert(handle != NULL);
	return (void*)wine_device_memory_from_handle((VkDeviceMemory)handle)->host_memory;
}

void* wine_unwrap_queue(void* handle) {
	assert(handle != NULL);
	return wine_queue_from_handle(handle)->host_queue;
}
