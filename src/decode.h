#pragma once
#include "common.h"
#include <wincodec.h>
namespace viewer {
enum class JpegBackend { Automatic, Wic, Turbo };
struct DecodeOptions {
    JpegBackend jpeg = JpegBackend::Automatic;
    // Keep a native JPEG reduction at/above the viewport; Direct2D performs the final resize.
    bool nativeJpegSize = false;
};
// Owned and destroyed on the decoding thread, before CoUninitialize.
class ImageDecoder {
  public:
    ~ImageDecoder();
    ImageDecoder() = default;
    ImageDecoder(const ImageDecoder&) = delete;
    ImageDecoder& operator=(const ImageDecoder&) = delete;
    std::shared_ptr<Frame> decode(BytePtr bytes, uint32_t maxWidth, uint32_t maxHeight, uint64_t budget,
                                  const Cancel& cancel = {}, DecodeOptions options = {});

  private:
    ComPtr<IWICImagingFactory> factory_;
    void* jpeg_ = nullptr;
};
std::shared_ptr<Frame> decodeImage(BytePtr bytes, uint32_t maxWidth, uint32_t maxHeight, uint64_t budget,
                                   const Cancel& cancel = {}, DecodeOptions options = {});
} // namespace viewer
