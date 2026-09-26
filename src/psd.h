#pragma once
#include "common.h"
namespace viewer {
struct PsdImage {
    std::shared_ptr<Frame> frame; // Straight BGRA; the common WIC path handles ICC and premultiplication.
    Bytes icc;
    bool grayscale = false;
};
PsdImage decodePsd(std::span<const uint8_t> bytes, uint32_t maxWidth, uint32_t maxHeight, uint64_t budget,
                   const Cancel& cancel);
} // namespace viewer
