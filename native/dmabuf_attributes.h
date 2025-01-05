#include <cstdint>

struct DmabufAttributes_t {
    void *pNext;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipLevels;
    uint32_t layers;
    uint32_t samples;
    char format[4];
    char _[4];
    uint64_t drmFormatModifer;
    uint32_t plane_count;
    // This part is repeated for each plane. We only support a single plane so
    // we just have a single repetition
    uint32_t offset;
    uint32_t rowpitch;
    int32_t fd;
};

