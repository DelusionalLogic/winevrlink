#pragma once
// We can't include an actual set of vulkan headers here since we have
// conflicting definitions between the host vulkan header files and the
// winevulkan ones. The implementation c file wants to use the winevulkan once
// such that it can unwrap the host vulkan device. Fortunately the handles are
// just pointers
#ifdef __cplusplus
extern "C" {
#endif

void* wine_unwrap_phys_dev(void* handle);
void* wine_unwrap_instance(void* handle);
void* wine_unwrap_device(void* handle);
void* wine_unwrap_dev_mem(void* handle);
void* wine_unwrap_queue(void* handle);

#ifdef __cplusplus
}
#endif
