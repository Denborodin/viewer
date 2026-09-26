#include "decode.h"
#include <iostream>
#include <cmath>
using namespace viewer;
int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::cerr << "Usage: ViewerJpegBench image width height runs [backend]\n";
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto input = readFile(argv[1]);
        uint32_t w = std::max(1, _wtoi(argv[2])), h = std::max(1, _wtoi(argv[3]));
        int runs = std::clamp(_wtoi(argv[4]), 1, 200);
        std::vector<double> times;
        std::shared_ptr<Frame> frame;
        DecodeOptions options;
        std::wstring mode = argc > 5 ? argv[5] : L"auto";
        if (mode.rfind(L"wic", 0) == 0)
            options.jpeg = JpegBackend::Wic;
        if (mode.rfind(L"turbo", 0) == 0)
            options.jpeg = JpegBackend::Turbo;
        options.nativeJpegSize = mode.find(L"gpu") != std::wstring::npos;
        {
            ImageDecoder decoder;
            for (int i = 0; i < runs + 3; ++i) {
                auto begin = std::chrono::steady_clock::now();
                frame = decoder.decode(input, w, h, 128 * MiB, {}, options);
                auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                              .count();
                if (i >= 3)
                    times.push_back(ms);
            }
        }
        double sum = 0;
        for (auto ms : times)
            sum += ms;
        std::sort(times.begin(), times.end());
        std::cout << "{\"runs\":" << runs << ",\"p50_ms\":" << times[times.size() / 2]
                  << ",\"p95_ms\":" << times[(size_t)std::ceil(times.size() * .95) - 1]
                  << ",\"mean_ms\":" << sum / times.size() << ",\"width\":" << frame->width
                  << ",\"height\":" << frame->height << "}\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
