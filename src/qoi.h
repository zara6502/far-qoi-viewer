
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct QoiImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t channels = 0;
  uint8_t colorspace = 0;
  std::vector<uint8_t> rgba;
};

enum class QoiErrorStage : uint8_t {
  None,
  File,
  Header,
  Trailer,
  StripScan,
  StripDecode,
  StandardDecode,
  Memory
};

enum class QoiErrorCode : uint8_t {
  None,
  FileOpenFailed,
  FileSizeFailed,
  FileReadFailed,
  FileTooLarge,
  TooShort,
  BadMagic,
  InvalidDimensions,
  InvalidChannels,
  InvalidColorSpace,
  PixelLimitExceeded,
  InvalidEndMarker,
  InvalidStripHeight,
  UnexpectedEnd,
  TruncatedRgb,
  TruncatedRgba,
  TruncatedLuma,
  InvalidOpcode,
  RunExceedsImage,
  RunCrossesStripBoundary,
  StripMissingAbsolutePixel,
  ExtraEncodedBytes,
  WrongStripCount,
  InvalidDecodeRange,
  PixelCountOverflow,
  StripDecodeFailed,
  AllocationFailed,
  UnknownDecodeFailure
};

struct QoiDecodeError {
  QoiErrorStage stage = QoiErrorStage::None;
  QoiErrorCode code = QoiErrorCode::None;
  size_t offset = 0;
  uint32_t strip = 0;
};

bool qoi_decode(const uint8_t *data, size_t size, QoiImage &out,
                QoiDecodeError *error = nullptr);
