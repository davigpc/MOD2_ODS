#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void on_signal(int) {
    g_stopRequested = 1;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " [--port PORT] [--db PATH] [--media-dir PATH]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string dbPath = "/tmp/s4_media_evidence.db";
    std::string mediaDir = "/tmp/s4_media_evidence";

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto read_value = [&](const char* flag) -> bool {
            if (argument == flag && i + 1 < argc) {
                ++i;
                return true;
            }
            return false;
        };
        if (read_value("--port")) {
            port = std::stoi(argv[i]);
        } else if (read_value("--db")) {
            dbPath = argv[i];
        } else if (read_value("--media-dir")) {
            mediaDir = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    std::error_code errorCode;
    std::filesystem::create_directories(mediaDir, errorCode);
    if (errorCode) {
        std::cerr << "cannot create media directory '" << mediaDir << "': "
                  << errorCode.message() << '\n';
        return 1;
    }

    auto repository = std::make_shared<ods::s4::infrastructure::SQLiteMediaClipRepository>(dbPath);

    // S4.1 — ring buffer real em /dev/shm. A fonte sintetica mantem o demo
    // rodando sem camera; na Jetson, troque por GStreamerAppsinkFrameSource
    // com jetsonCsiH264Pipeline(). Nada mais nesta funcao muda: o
    // ExtractClipUseCase so conhece a porta IMediaBufferReader.
    ods::s4::application::RingBufferConfig bufferConfig;
    bufferConfig.cameraId = "cam0";
    bufferConfig.windowSeconds = 30.0;
    bufferConfig.bitrateBps = 134400;
    bufferConfig.width = 320;
    bufferConfig.height = 240;
    // Daemon: apos uma queda, o /dev/shm da execucao anterior pode ter ficado
    // para tras e impediria a subida. Uma instancia por camera, entao limpar e
    // o comportamento correto aqui.
    bufferConfig.replaceStaleSegment = true;

    ods::s4::infrastructure::SyntheticSourceOptions sourceOptions;
    sourceOptions.fps = 15.0;
    sourceOptions.gop = 15;
    sourceOptions.keyframeBytes = 6000;
    sourceOptions.deltaBytes = 1200;
    sourceOptions.sessionId = "exec-demo-1";

    std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade> mediaBuffer;
    try {
        mediaBuffer = std::make_shared<ods::s4::infrastructure::RingBufferRAMFacade>(
            bufferConfig,
            std::make_unique<ods::s4::infrastructure::SyntheticFrameSource>(sourceOptions)
        );
    } catch (const ods::s4::domain::ODSBaseException& error) {
        std::cerr << "cannot start the S4.1 ring buffer: " << error.what() << std::endl;
        return 1;
    }
    auto fileStorage = std::make_shared<ods::s4::infrastructure::FileStorage>();
    auto extractClip = std::make_shared<ods::s4::application::ExtractClipUseCase>(
        repository, mediaBuffer, fileStorage
    );

    mediaBuffer->startCapture("synthetic-source");

    ods::s4::presentation::ClipDescriptorHttpServer server(repository, extractClip, mediaDir);
    if (!server.bind(port)) {
        std::cerr << "cannot bind to port " << port << '\n';
        return 1;
    }
    port = server.port();
    if (!server.start()) {
        std::cerr << "cannot start HTTP server\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "S4 Media Evidence listening on http://127.0.0.1:" << port
              << "/api/v1/clips/{id}\n";
    std::cout << "Simulate an event: POST http://127.0.0.1:" << port
              << "/api/v1/events\n";
    std::cout << "Recording synthetic feed; extracting demo clip...\n";
    std::cout.flush();

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    const auto end = std::chrono::system_clock::now();
    const auto start = end - std::chrono::seconds(1);
    const std::string outputPath = (std::filesystem::path(mediaDir) / "event-demo.mp4").string();
    try {
        const auto clip = extractClip->execute("event-demo", ods::s4::domain::TimeWindow(start, end), outputPath);
        std::cout << "clip_id: " << clip.clipId() << '\n';
        std::cout << "file:    " << clip.filePath() << '\n';
        std::cout << "sha256:  " << clip.sha256Hash() << '\n';
        std::cout << "GET http://127.0.0.1:" << port << "/api/v1/clips/"
                  << clip.clipId() << "\n";
        std::cout.flush();
    } catch (const std::exception& error) {
        std::cerr << "clip extraction failed: " << error.what() << '\n';
    }

    std::cout << "Idle until SIGINT...\n";
    std::cout.flush();

    while (g_stopRequested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mediaBuffer->stopCapture();
    server.stop();
    return 0;
}