#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

struct QoiImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
    uint8_t colorspace = 0;
    std::vector<uint8_t> rgba; // always RGBA8888
};

bool qoi_decode(const uint8_t* data, size_t size, QoiImage& out);
