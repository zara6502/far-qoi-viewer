#include "qoi.h"

#include <cstring>
#include <limits>

namespace
{
constexpr uint8_t QOI_OP_INDEX = 0x00;
constexpr uint8_t QOI_OP_DIFF  = 0x40;
constexpr uint8_t QOI_OP_LUMA  = 0x80;
constexpr uint8_t QOI_OP_RUN   = 0xC0;
constexpr uint8_t QOI_OP_RGB   = 0xFE;
constexpr uint8_t QOI_OP_RGBA  = 0xFF;

static inline uint8_t hash_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return static_cast<uint8_t>((r * 3u + g * 5u + b * 7u + a * 11u) & 63u);
}

static inline uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) |
           (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) |
           uint32_t(p[3]);
}

static inline uint8_t clamp_add(uint8_t base, int delta)
{
    return static_cast<uint8_t>(base + delta);
}
}

bool qoi_decode(const uint8_t* data, size_t size, QoiImage& out)
{
    out = {};

    // Header (14) + 8-byte end marker.
    if (!data || size < 22) return false;
    if (std::memcmp(data, "qoif", 4) != 0) return false;

    const uint32_t w = be32(data + 4);
    const uint32_t h = be32(data + 8);
    const uint8_t channels = data[12];
    const uint8_t colorspace = data[13];

    if (!w || !h || (channels != 3 && channels != 4))
        return false;
    if (colorspace > 1)
        return false;

    const uint64_t pixels64 = uint64_t(w) * uint64_t(h);
    if (pixels64 > (std::numeric_limits<size_t>::max() / 4))
        return false;

    const size_t pixels = static_cast<size_t>(pixels64);
    const size_t outputBytes = pixels * 4;

    out.width = w;
    out.height = h;
    out.channels = channels;
    out.colorspace = colorspace;
    out.rgba.resize(outputBytes);

    uint8_t index[64][4] = {};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t p = 14;
    size_t dst = 0;
    size_t run = 0;

    for (size_t px = 0; px < pixels; ++px)
    {
        if (run)
        {
            --run;
        }
        else
        {
            if (p >= size) return false;
            const uint8_t op = data[p++];

            if (op == QOI_OP_RGB)
            {
                if (p + 3 > size) return false;
                r = data[p++];
                g = data[p++];
                b = data[p++];
            }
            else if (op == QOI_OP_RGBA)
            {
                if (p + 4 > size) return false;
                r = data[p++];
                g = data[p++];
                b = data[p++];
                a = data[p++];
            }
            else
            {
                switch (op & 0xC0)
                {
                    case QOI_OP_INDEX:
                    {
                        const uint8_t* q = index[op & 63];
                        r = q[0]; g = q[1]; b = q[2]; a = q[3];
                        break;
                    }

                    case QOI_OP_DIFF:
                    {
                        r = clamp_add(r, ((op >> 4) & 3) - 2);
                        g = clamp_add(g, ((op >> 2) & 3) - 2);
                        b = clamp_add(b, (op & 3) - 2);
                        break;
                    }

                    case QOI_OP_LUMA:
                    {
                        if (p >= size) return false;
                        const uint8_t b2 = data[p++];
                        const int dg = int((op & 0x3F)) - 32;
                        const int dr_dg = int((b2 >> 4) & 0x0F) - 8;
                        const int db_dg = int(b2 & 0x0F) - 8;
                        g = clamp_add(g, dg);
                        r = clamp_add(r, dg + dr_dg);
                        b = clamp_add(b, dg + db_dg);
                        break;
                    }

                    case QOI_OP_RUN:
                        run = (op & 63) + 1;
                        --run;
                        break;

                    default:
                        return false;
                }
            }

            const uint8_t slot = hash_pixel(r, g, b, a);
            index[slot][0] = r;
            index[slot][1] = g;
            index[slot][2] = b;
            index[slot][3] = a;
        }

        out.rgba[dst++] = r;
        out.rgba[dst++] = g;
        out.rgba[dst++] = b;
        out.rgba[dst++] = a;
    }

    // QOI requires the eight-byte end marker 00 00 00 00 00 00 00 01.
    if (p + 8 > size) return false;
    static constexpr uint8_t endMarker[8] = {0,0,0,0,0,0,0,1};
    if (std::memcmp(data + p, endMarker, 8) != 0)
        return false;

    return true;
}
