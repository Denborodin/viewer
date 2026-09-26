#include "pipeline.h"
#include "renderer.h"
#include <cmath>
#include <fstream>
#include <iostream>
using namespace viewer;
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ViewerBench archive [iterations=20]\n";
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        int runs = argc > 2 ? _wtoi(argv[2]) : 20;
        runs = std::clamp(runs, 1, 100);
        std::vector<double> opening, first, cached, prepared;
        uint64_t totalRead = 0;
        Settings settings;
        settings.diskBytes = 0;
        auto root = dataDirectory() / L"bench" / std::to_wstring(GetCurrentProcessId());
        auto state = std::make_shared<StateStore>(root);
        for (int n = 0; n < runs; ++n) {
            auto metrics = std::make_shared<Metrics>();
            auto cache = std::make_shared<RawCache>(state, settings, metrics);
            auto begin = std::chrono::steady_clock::now();
            auto source = openSource(fs::absolute(argv[1]), cache, metrics, {});
            auto listed = std::chrono::steady_clock::now();
            if (source->entries().empty())
                throw Error("No images");
            auto id = source->entries()[0].id;
            auto f =
                decodeImage(source->read(id, {}), 1920, 1080, 128 * MiB, {}, {JpegBackend::Automatic, true});
            auto shown = std::chrono::steady_clock::now();
            auto b = source->read(id, {});
            auto warm = std::chrono::steady_clock::now();
            opening.push_back(std::chrono::duration<double, std::milli>(listed - begin).count());
            first.push_back(std::chrono::duration<double, std::milli>(shown - begin).count());
            cached.push_back(std::chrono::duration<double, std::milli>(warm - shown).count());
            totalRead += metrics->archiveReads;
        }
        // Warm two decoded frames, then measure selection -> Direct2D EndDraw.
        HWND window = CreateWindowExW(0, L"STATIC", L"Viewer benchmark", WS_OVERLAPPEDWINDOW, 0, 0, 1200, 820,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window)
            throw Error("Benchmark window");
        {
            Renderer renderer(window);
            ViewModel model;
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Event> events;
            ImagePipeline pipeline(state, [&](std::unique_ptr<Event> event) {
                std::lock_guard lock(mutex);
                events.push_back(std::move(*event));
                cv.notify_one();
            });
            auto receive = [&](uint64_t generation) {
                std::unique_lock lock(mutex);
                auto ready = [&] {
                    return std::any_of(events.begin(), events.end(), [&](const Event& e) {
                        return e.generation >= generation &&
                               (e.type == Event::Type::FrameReady || e.type == Event::Type::Error);
                    });
                };
                if (!cv.wait_for(lock, std::chrono::seconds(30), ready))
                    throw Error("Pipeline benchmark timeout");
                std::shared_ptr<Frame> frame;
                for (auto& e : events) {
                    if (e.type == Event::Type::Error && e.generation >= generation)
                        throw Error(utf8(e.message));
                    if (e.type == Event::Type::FrameReady && e.generation >= generation)
                        frame = e.frame;
                }
                events.clear();
                return frame;
            };
            pipeline.configure(settings);
            pipeline.resize(1920, 1080);
            pipeline.open(fs::absolute(argv[1]));
            model.frame = receive(pipeline.generation());
            renderer.draw(model);
            for (int n = 0; n < runs + 6; ++n) {
                auto begin = std::chrono::steady_clock::now();
                auto generation = pipeline.select(n % 2);
                model.frame = receive(generation);
                renderer.draw(model);
                if (n >= 6)
                    prepared.push_back(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                            .count());
            }
        }
        DestroyWindow(window);
        auto percentile = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            return values[(size_t)std::ceil(values.size() * .95) - 1];
        };
        std::cout
            << "{\"runs\":" << runs << ",\"catalog_p95_ms\":" << percentile(opening)
            << ",\"first_decoded_frame_p95_ms\":" << percentile(first)
            << ",\"cached_bytes_p95_ms\":" << percentile(cached)
            << ",\"prepared_frame_submit_p95_ms\":" << percentile(prepared)
            << ",\"mean_archive_read_bytes\":" << totalRead / runs
            << ",\"os_cache\":\"uncontrolled; warm after initial run\",\"app_cache\":\"empty per run\"}\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
