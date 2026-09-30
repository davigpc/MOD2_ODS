#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/application/use_cases/purge_media.hpp"
#include "s4/domain/strategies/disk_quota_fifo_strategy.hpp"
#include "s4/domain/strategies/lgpd_retention_strategy.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/retention/json_lines_purge_log.hpp"
#include "s4/infrastructure/retention/purge_daemon.hpp"
#include "s4/infrastructure/retention/statvfs_disk_usage_provider.hpp"
#include "s4/infrastructure/retention/system_clock.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void on_signal(int) {
    g_stopRequested = 1;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " [--port PORT] [--db PATH] [--media-dir PATH]\n"
                 "       [--retention-days DAYS] [--quota-trigger RATIO] [--quota-target RATIO]\n"
                 "       [--sweep-interval SECONDS] [--disk-check-interval SECONDS]"
                 " [--purge-log PATH]\n";
}

// S4.3 — parametros do expurgo. Os padroes sao os do guia: 7 dias (LGPD) e
// gatilho de 85% do NVMe.
struct RetentionOptions {
    double retentionDays{7.0};
    double quotaTrigger{ods::s4::domain::QuotaPolicy::kDefaultTriggerRatio};
    double quotaTarget{ods::s4::domain::QuotaPolicy::kDefaultTargetRatio};
    long long sweepIntervalSeconds{3600};
    long long diskCheckIntervalSeconds{30};
    std::string purgeLogPath;
};

std::unique_ptr<ods::s4::infrastructure::PurgeDaemon> make_purge_daemon(
    const RetentionOptions& options,
    std::shared_ptr<ods::s4::domain::IMediaClipRepository> repository,
    std::shared_ptr<ods::s4::domain::IFileStorage> fileStorage,
    const std::string& mediaDir
) {
    using namespace ods::s4;
    const domain::RetentionPolicy retention(std::chrono::seconds(
        static_cast<long long>(options.retentionDays * 24 * 3600)));
    const domain::QuotaPolicy quota(options.quotaTrigger, options.quotaTarget);
    auto diskUsage = std::make_shared<infrastructure::StatvfsDiskUsageProvider>(mediaDir);

    auto purgeMedia = std::make_shared<application::PurgeMediaUseCase>(
        std::move(repository), std::move(fileStorage), diskUsage,
        std::make_shared<infrastructure::JsonLinesPurgeLog>(options.purgeLogPath),
        std::make_shared<infrastructure::SystemClock>(),
        std::vector<std::shared_ptr<const domain::IRetentionStrategy>>{
            std::make_shared<domain::LgpdRetentionStrategy>(retention),
            std::make_shared<domain::DiskQuotaFifoStrategy>(quota),
        });

    infrastructure::PurgeDaemonConfig config;
    config.retentionSweepInterval = std::chrono::seconds(options.sweepIntervalSeconds);
    config.diskCheckInterval = std::chrono::seconds(options.diskCheckIntervalSeconds);
    return std::make_unique<infrastructure::PurgeDaemon>(purgeMedia, diskUsage, quota, config);
}

} // namespace

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string dbPath = "/tmp/s4_media_evidence.db";
    std::string mediaDir = "/tmp/s4_media_evidence";
    RetentionOptions retentionOptions;

    try {
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
            } else if (read_value("--retention-days")) {
                retentionOptions.retentionDays = std::stod(argv[i]);
            } else if (read_value("--quota-trigger")) {
                retentionOptions.quotaTrigger = std::stod(argv[i]);
            } else if (read_value("--quota-target")) {
                retentionOptions.quotaTarget = std::stod(argv[i]);
            } else if (read_value("--sweep-interval")) {
                retentionOptions.sweepIntervalSeconds = std::stoll(argv[i]);
            } else if (read_value("--disk-check-interval")) {
                retentionOptions.diskCheckIntervalSeconds = std::stoll(argv[i]);
            } else if (read_value("--purge-log")) {
                retentionOptions.purgeLogPath = argv[i];
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }
    } catch (const std::logic_error&) {
        // std::stoi/stod com texto nao numerico ou fora da faixa.
        print_usage(argv[0]);
        return 1;
    }
    if (retentionOptions.sweepIntervalSeconds <= 0 || retentionOptions.diskCheckIntervalSeconds <= 0) {
        std::cerr << "--sweep-interval and --disk-check-interval must be positive\n";
        return 1;
    }
    if (retentionOptions.purgeLogPath.empty()) {
        // Ao lado do banco: no container, os dois ficam no volume /data.
        retentionOptions.purgeLogPath =
            (std::filesystem::path(dbPath).parent_path() / "s4_purge_log.jsonl").string();
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

    // S4.3 — expurgo automatico (LGPD + cota do NVMe), em thread propria.
    std::unique_ptr<ods::s4::infrastructure::PurgeDaemon> purgeDaemon;
    try {
        purgeDaemon = make_purge_daemon(retentionOptions, repository, fileStorage, mediaDir);
    } catch (const ods::s4::domain::ODSBaseException& error) {
        std::cerr << "invalid retention settings: " << error.what() << std::endl;
        return 1;
    }

    mediaBuffer->startCapture("synthetic-source");
    purgeDaemon->start();

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
    std::cout << "Purge daemon: retention " << retentionOptions.retentionDays << " d, quota "
              << retentionOptions.quotaTrigger * 100 << "% -> " << retentionOptions.quotaTarget * 100
              << "%, log " << retentionOptions.purgeLogPath << '\n';
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

    purgeDaemon->stop();
    mediaBuffer->stopCapture();
    server.stop();
    return 0;
}