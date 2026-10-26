#include "png.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace amica_engine {

namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[160];
        std::snprintf(text, sizeof(text), "PNG encoding: %s failed: HRESULT 0x%08X", what, static_cast<unsigned>(hr));
        throw std::runtime_error(text);
    }
}

// Requests arrive on HTTP worker threads, so COM is entered for each call.
class ComScope {
public:
    ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) check(hr_, "CoInitializeEx");
    }
    ~ComScope() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT hr_;
};

}  // namespace

std::string encode_png(std::size_t width, std::size_t height, const std::vector<std::uint8_t>& rgb) {
    if (width == 0 || height == 0 || rgb.size() != width * height * 3) {
        throw std::runtime_error("PNG encoding: the pixels do not match the image size");
    }
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "CoCreateInstance(WICImagingFactory)");
    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "CreateStreamOnHGlobal");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "IWICBitmapEncoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    check(frame->Initialize(nullptr), "IWICBitmapFrameEncode::Initialize");
    check(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)), "SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format), "SetPixelFormat");
    if (format != GUID_WICPixelFormat24bppBGR) {
        throw std::runtime_error("PNG encoding: WIC does not accept 24-bit BGR pixels");
    }
    std::vector<std::uint8_t> bgr(rgb);
    for (std::size_t i = 0; i < bgr.size(); i += 3) std::swap(bgr[i], bgr[i + 2]);
    check(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width * 3), static_cast<UINT>(bgr.size()),
                             bgr.data()),
          "WritePixels");
    check(frame->Commit(), "IWICBitmapFrameEncode::Commit");
    check(encoder->Commit(), "IWICBitmapEncoder::Commit");

    STATSTG stat{};
    check(stream->Stat(&stat, STATFLAG_NONAME), "IStream::Stat");
    HGLOBAL memory = nullptr;
    check(GetHGlobalFromStream(stream.Get(), &memory), "GetHGlobalFromStream");
    const void* data = GlobalLock(memory);
    if (data == nullptr) throw std::runtime_error("PNG encoding: GlobalLock failed");
    std::string png(static_cast<const char*>(data), static_cast<std::size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    return png;
}

}  // namespace amica_engine
