
#include "qoi.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace {
constexpr uint8_t QOI_OP_INDEX = 0x00;
constexpr uint8_t QOI_OP_DIFF = 0x40;
constexpr uint8_t QOI_OP_LUMA = 0x80;
constexpr uint8_t QOI_OP_RUN = 0xC0;
constexpr uint8_t QOI_OP_RGB = 0xFE;
constexpr uint8_t QOI_OP_RGBA = 0xFF;

constexpr uint8_t QOI_END_STANDARD[8] = {0, 0, 0, 0, 0, 0, 0, 1};

// Compatibility with the previous FarQoiViewer.
constexpr uint8_t QOI_END_LEGACY[8] = {255, 0, 0, 0, 0, 0, 0, 1};

constexpr size_t QOI_HEADER_SIZE = 14;
constexpr size_t QOI_END_SIZE = 8;
constexpr uint64_t QOI_PIXELS_MAX = 400000000ULL;

struct QoiTrailer {
  bool valid = false;
  bool qoimt = false;
  uint8_t stripHeight = 0;
  uint8_t threads = 0;
};

struct StripRange {
  size_t begin = 0;
  size_t end = 0;
  uint32_t y = 0;
  uint32_t height = 0;
};

static void set_error(QoiDecodeError *error, QoiErrorStage stage,
                      QoiErrorCode code, size_t offset = 0,
                      uint32_t strip = 0) {
  if (!error || error->code != QoiErrorCode::None)
    return;

  error->stage = stage;
  error->code = code;
  error->offset = offset;
  error->strip = strip;
}

static inline uint8_t hash_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint8_t>((r * 3u + g * 5u + b * 7u + a * 11u) & 63u);
}

static inline uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline uint8_t wrap_add(uint8_t base, int delta) {
  return static_cast<uint8_t>(int(base) + delta);
}

static QoiTrailer read_trailer(const uint8_t *data, size_t size) {
  QoiTrailer result{};

  if (!data || size < QOI_HEADER_SIZE + QOI_END_SIZE)
    return result;

  const uint8_t *marker = data + size - QOI_END_SIZE;

  if (std::memcmp(marker, QOI_END_STANDARD, QOI_END_SIZE) == 0) {
    result.valid = true;
    return result;
  }

  if (std::memcmp(marker, QOI_END_LEGACY, QOI_END_SIZE) == 0) {
    result.valid = true;

    // Ambiguous with QOIMT strip=255, threads=0.
    // Try QOIMT first for tall images; otherwise retain legacy behavior.
    if (be32(data + 8) > 255) {
      result.qoimt = true;
      result.stripHeight = 255;
      result.threads = 0;
    }

    return result;
  }

  // QOIMT footer:
  // [strip_height][threads][0][0][0][0][0][1]
  if (marker[0] != 0 && marker[7] == 1) {
    bool reservedZero = true;

    for (size_t i = 2; i < 7; ++i) {
      if (marker[i] != 0) {
        reservedZero = false;
        break;
      }
    }

    if (reservedZero) {
      result.valid = true;
      result.qoimt = true;
      result.stripHeight = marker[0];
      result.threads = marker[1];
    }
  }

  return result;
}

static unsigned int resolve_threads(uint8_t embedded, size_t jobs) {
  if (jobs <= 1)
    return 1;

  unsigned int n = embedded;

  if (n == 0)
    n = std::thread::hardware_concurrency();

  if (n == 0)
    n = 1;

  unsigned int hw = std::thread::hardware_concurrency();

  if (hw == 0)
    hw = 1;

  n = std::min(n, hw);
  n = std::min(n, static_cast<unsigned int>(jobs));

  return std::max(1u, n);
}

static bool decode_commands(const uint8_t *data, size_t begin, size_t end,
                            uint32_t width, uint32_t height, uint8_t *rgba,
                            QoiDecodeError *error, QoiErrorStage stage,
                            uint32_t strip) {
  if (!data || !rgba || begin > end) {
    set_error(error, stage, QoiErrorCode::InvalidDecodeRange, begin, strip);
    return false;
  }

  const uint64_t pixelCount64 = uint64_t(width) * height;

  if (pixelCount64 == 0 ||
      pixelCount64 > std::numeric_limits<size_t>::max() / 4) {
    set_error(error, stage, QoiErrorCode::PixelCountOverflow, begin, strip);
    return false;
  }

  const size_t pixelCount = static_cast<size_t>(pixelCount64);

  uint8_t index[64][4] = {};
  uint8_t r = 0, g = 0, b = 0, a = 255;

  size_t p = begin;
  size_t run = 0;

  for (size_t px = 0; px < pixelCount; ++px) {
    if (run > 0) {
      --run;
    } else {
      if (p >= end) {
        set_error(error, stage, QoiErrorCode::UnexpectedEnd, p, strip);
        return false;
      }

      const size_t opOffset = p;
      const uint8_t op = data[p++];

      if (op == QOI_OP_RGB) {
        if (end - p < 3) {
          set_error(error, stage, QoiErrorCode::TruncatedRgb, opOffset, strip);
          return false;
        }

        r = data[p++];
        g = data[p++];
        b = data[p++];
      } else if (op == QOI_OP_RGBA) {
        if (end - p < 4) {
          set_error(error, stage, QoiErrorCode::TruncatedRgba, opOffset, strip);
          return false;
        }

        r = data[p++];
        g = data[p++];
        b = data[p++];
        a = data[p++];
      } else {
        switch (op & 0xC0) {
        case QOI_OP_INDEX: {
          const uint8_t *q = index[op & 63];
          r = q[0];
          g = q[1];
          b = q[2];
          a = q[3];
          break;
        }

        case QOI_OP_DIFF: {
          r = wrap_add(r, int((op >> 4) & 3) - 2);
          g = wrap_add(g, int((op >> 2) & 3) - 2);
          b = wrap_add(b, int(op & 3) - 2);
          break;
        }

        case QOI_OP_LUMA: {
          if (p >= end) {
            set_error(error, stage, QoiErrorCode::TruncatedLuma, opOffset,
                      strip);
            return false;
          }

          const uint8_t b2 = data[p++];
          const int dg = int(op & 0x3F) - 32;
          const int dr_dg = int((b2 >> 4) & 0x0F) - 8;
          const int db_dg = int(b2 & 0x0F) - 8;

          g = wrap_add(g, dg);
          r = wrap_add(r, dg + dr_dg);
          b = wrap_add(b, dg + db_dg);
          break;
        }

        case QOI_OP_RUN: {
          run = size_t(op & 0x3F);

          if (run > pixelCount - px - 1) {
            set_error(error, stage, QoiErrorCode::RunExceedsImage, opOffset,
                      strip);
            return false;
          }
          break;
        }

        default:
          set_error(error, stage, QoiErrorCode::InvalidOpcode, opOffset, strip);
          return false;
        }
      }
    }

    const uint8_t slot = hash_pixel(r, g, b, a);

    index[slot][0] = r;
    index[slot][1] = g;
    index[slot][2] = b;
    index[slot][3] = a;

    const size_t dst = px * 4;

    rgba[dst + 0] = r;
    rgba[dst + 1] = g;
    rgba[dst + 2] = b;
    rgba[dst + 3] = a;
  }

  if (p != end || run != 0) {
    set_error(error, stage, QoiErrorCode::ExtraEncodedBytes, p, strip);
    return false;
  }

  return true;
}

static bool scan_strips(const uint8_t *data, size_t chunksEnd, uint32_t width,
                        uint32_t height, uint32_t stripHeight,
                        std::vector<StripRange> &strips,
                        QoiDecodeError *error) {
  strips.clear();

  if (!data || width == 0 || height == 0 || stripHeight == 0 ||
      stripHeight >= height) {
    set_error(error, QoiErrorStage::StripScan,
              QoiErrorCode::InvalidStripHeight);
    return false;
  }

  const uint64_t totalPixels = uint64_t(width) * height;

  size_t p = QOI_HEADER_SIZE;
  uint64_t decodedPixels = 0;

  uint32_t bandY = 0;
  uint32_t nextY = std::min(stripHeight, height);
  uint64_t nextBoundary = uint64_t(nextY) * width;

  size_t bandStart = p;

  const size_t expectedStrips =
      size_t(height / stripHeight) + (height % stripHeight != 0);

  strips.reserve(expectedStrips);

  while (decodedPixels < totalPixels) {
    const uint32_t currentStrip = static_cast<uint32_t>(strips.size() + 1);

    if (p >= chunksEnd) {
      set_error(error, QoiErrorStage::StripScan, QoiErrorCode::UnexpectedEnd, p,
                currentStrip);
      return false;
    }

    const size_t opOffset = p;
    const uint8_t op = data[p++];

    uint64_t produced = 1;

    if (op == QOI_OP_RGB) {
      if (chunksEnd - p < 3) {
        set_error(error, QoiErrorStage::StripScan, QoiErrorCode::TruncatedRgb,
                  opOffset, currentStrip);
        return false;
      }

      p += 3;
    } else if (op == QOI_OP_RGBA) {
      if (chunksEnd - p < 4) {
        set_error(error, QoiErrorStage::StripScan, QoiErrorCode::TruncatedRgba,
                  opOffset, currentStrip);
        return false;
      }

      p += 4;
    } else {
      switch (op & 0xC0) {
      case QOI_OP_INDEX:
      case QOI_OP_DIFF:
        break;

      case QOI_OP_LUMA:
        if (p >= chunksEnd) {
          set_error(error, QoiErrorStage::StripScan,
                    QoiErrorCode::TruncatedLuma, opOffset, currentStrip);
          return false;
        }
        ++p;
        break;

      case QOI_OP_RUN:
        produced = uint64_t(op & 0x3F) + 1;
        break;

      default:
        set_error(error, QoiErrorStage::StripScan, QoiErrorCode::InvalidOpcode,
                  opOffset, currentStrip);
        return false;
      }
    }

    if (produced > totalPixels - decodedPixels) {
      set_error(error, QoiErrorStage::StripScan, QoiErrorCode::RunExceedsImage,
                opOffset, currentStrip);
      return false;
    }

    decodedPixels += produced;

    if (decodedPixels == nextBoundary) {
      strips.push_back(StripRange{bandStart, p, bandY, nextY - bandY});

      bandStart = p;
      bandY = nextY;

      if (decodedPixels < totalPixels) {
        const uint32_t nextStrip = static_cast<uint32_t>(strips.size() + 1);

        if (p >= chunksEnd ||
            (data[p] != QOI_OP_RGB && data[p] != QOI_OP_RGBA)) {
          set_error(error, QoiErrorStage::StripScan,
                    QoiErrorCode::StripMissingAbsolutePixel, p, nextStrip);
          return false;
        }

        nextY = std::min(height, bandY + stripHeight);
        nextBoundary = uint64_t(nextY) * width;
      }
    } else if (decodedPixels > nextBoundary) {
      set_error(error, QoiErrorStage::StripScan,
                QoiErrorCode::RunCrossesStripBoundary, opOffset, currentStrip);
      return false;
    }
  }

  if (p != chunksEnd || bandStart != chunksEnd) {
    set_error(error, QoiErrorStage::StripScan, QoiErrorCode::ExtraEncodedBytes,
              p);
    return false;
  }

  if (strips.size() != expectedStrips) {
    set_error(error, QoiErrorStage::StripScan, QoiErrorCode::WrongStripCount,
              p);
    return false;
  }

  return true;
}

static bool decode_qoimt(const uint8_t *data, size_t size, uint32_t width,
                         uint32_t height, uint8_t channels, uint8_t colorspace,
                         const QoiTrailer &trailer, QoiImage &out,
                         QoiDecodeError *error) {
  if (!trailer.qoimt || trailer.stripHeight == 0 ||
      trailer.stripHeight >= height) {
    set_error(error, QoiErrorStage::StripScan, QoiErrorCode::InvalidStripHeight,
              size >= QOI_END_SIZE ? size - QOI_END_SIZE : 0);
    return false;
  }

  std::vector<StripRange> strips;

  if (!scan_strips(data, size - QOI_END_SIZE, width, height,
                   trailer.stripHeight, strips, error))
    return false;

  const uint64_t pixelCount = uint64_t(width) * height;

  if (pixelCount > std::numeric_limits<size_t>::max() / 4) {
    set_error(error, QoiErrorStage::Memory, QoiErrorCode::PixelCountOverflow);
    return false;
  }

  out = {};
  out.width = width;
  out.height = height;
  out.channels = channels;
  out.colorspace = colorspace;
  out.rgba.resize(static_cast<size_t>(pixelCount) * 4);

  const unsigned int workers = resolve_threads(trailer.threads, strips.size());

  std::atomic<size_t> next{0};
  std::vector<uint8_t> failed(strips.size(), 0);
  std::vector<QoiDecodeError> stripErrors(strips.size());

  auto worker = [&]() {
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);

      if (i >= strips.size())
        break;

      const StripRange &strip = strips[i];

      uint8_t *dst = out.rgba.data() + size_t(strip.y) * width * 4;

      if (!decode_commands(data, strip.begin, strip.end, width, strip.height,
                           dst, &stripErrors[i], QoiErrorStage::StripDecode,
                           static_cast<uint32_t>(i + 1))) {
        failed[i] = 1;
      }
    }
  };

  std::vector<std::thread> pool;

  try {
    pool.reserve(workers > 0 ? workers - 1 : 0);

    for (unsigned int i = 1; i < workers; ++i) {
      try {
        pool.emplace_back(worker);
      } catch (...) {
        break;
      }
    }
  } catch (...) {
    // The calling thread can still decode the remaining strips.
  }

  worker();

  for (auto &thread : pool) {
    if (thread.joinable())
      thread.join();
  }

  for (size_t i = 0; i < failed.size(); ++i) {
    if (failed[i]) {
      if (error && error->code == QoiErrorCode::None)
        *error = stripErrors[i];

      return false;
    }
  }

  return true;
}

static bool decode_serial(const uint8_t *data, size_t size, uint32_t width,
                          uint32_t height, uint8_t channels, uint8_t colorspace,
                          QoiImage &out, QoiDecodeError *error) {
  out = {};

  const uint64_t pixelCount64 = uint64_t(width) * height;

  if (pixelCount64 == 0 ||
      pixelCount64 > std::numeric_limits<size_t>::max() / 4) {
    set_error(error, QoiErrorStage::StandardDecode,
              QoiErrorCode::PixelCountOverflow);
    return false;
  }

  const size_t chunksEnd = size - QOI_END_SIZE;

  out.width = width;
  out.height = height;
  out.channels = channels;
  out.colorspace = colorspace;
  out.rgba.resize(static_cast<size_t>(pixelCount64) * 4);

  return decode_commands(data, QOI_HEADER_SIZE, chunksEnd, width, height,
                         out.rgba.data(), error, QoiErrorStage::StandardDecode,
                         0);
}
} // namespace

bool qoi_decode(const uint8_t *data, size_t size, QoiImage &out,
                QoiDecodeError *error) {
  out = {};

  if (error)
    *error = {};

  if (!data || size < QOI_HEADER_SIZE + QOI_END_SIZE) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::TooShort, size);
    return false;
  }

  if (std::memcmp(data, "qoif", 4) != 0) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::BadMagic, 0);
    return false;
  }

  const uint32_t width = be32(data + 4);
  const uint32_t height = be32(data + 8);
  const uint8_t channels = data[12];
  const uint8_t colorspace = data[13];

  if (!width || !height) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::InvalidDimensions, 4);
    return false;
  }

  if (channels != 3 && channels != 4) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::InvalidChannels, 12);
    return false;
  }

  if (colorspace > 1) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::InvalidColorSpace,
              13);
    return false;
  }

  const uint64_t pixels64 = uint64_t(width) * uint64_t(height);

  if (pixels64 >= QOI_PIXELS_MAX ||
      pixels64 > std::numeric_limits<size_t>::max() / 4) {
    set_error(error, QoiErrorStage::Header, QoiErrorCode::PixelLimitExceeded,
              4);
    return false;
  }

  const QoiTrailer trailer = read_trailer(data, size);

  if (!trailer.valid) {
    set_error(error, QoiErrorStage::Trailer, QoiErrorCode::InvalidEndMarker,
              size - QOI_END_SIZE);
    return false;
  }

  QoiDecodeError qoimtError{};

  if (trailer.qoimt && trailer.stripHeight > 0 &&
      trailer.stripHeight < height) {
    try {
      if (decode_qoimt(data, size, width, height, channels, colorspace, trailer,
                       out, &qoimtError)) {
        return true;
      }
    } catch (const std::bad_alloc &) {
      set_error(&qoimtError, QoiErrorStage::Memory,
                QoiErrorCode::AllocationFailed);
    } catch (...) {
      set_error(&qoimtError, QoiErrorStage::StripDecode,
                QoiErrorCode::UnknownDecodeFailure);
    }

    out = {};
  } else if (trailer.qoimt) {
    set_error(&qoimtError, QoiErrorStage::StripScan,
              QoiErrorCode::InvalidStripHeight, size - QOI_END_SIZE);
  }

  // Keep the normal QOI decoder as a fallback.
  QoiDecodeError serialError{};

  try {
    if (decode_serial(data, size, width, height, channels, colorspace, out,
                      &serialError)) {
      return true;
    }
  } catch (const std::bad_alloc &) {
    set_error(&serialError, QoiErrorStage::Memory,
              QoiErrorCode::AllocationFailed);
  } catch (...) {
    set_error(&serialError, QoiErrorStage::StandardDecode,
              QoiErrorCode::UnknownDecodeFailure);
  }

  out = {};

  if (error) {
    if (trailer.qoimt && qoimtError.code != QoiErrorCode::None)
      *error = qoimtError;
    else if (serialError.code != QoiErrorCode::None)
      *error = serialError;
    else
      *error = {QoiErrorStage::StandardDecode,
                QoiErrorCode::UnknownDecodeFailure, 0, 0};
  }

  return false;
}
