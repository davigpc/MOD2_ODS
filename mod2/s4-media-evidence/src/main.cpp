#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#if ODS_S4_WITH_GSTREAMER
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/gstreamer_mp4_muxer.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void on_signal(int) {
    g_stopRequested = 1;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " [--port PORT] [--db PATH] [--media-dir PATH]\n"
                 "       [--source FILE] [--scenario] [--camera ID]\n"
                 "       [--window-seconds N] [--bitrate BPS]\n"
                 "\n"
                 "  --source FILE     replay de um MP4 H.264 existente, em vez da\n"
                 "                    fonte sintetica. E o caminho que produz um clipe\n"
                 "                    MP4 de verdade a partir de video de verdade.\n"
                 "  --scenario        extrai um clipe de demonstracao na subida e\n"
                 "                    imprime o descriptor. Sem este flag o daemon so\n"
                 "                    serve a API.\n"
                 "  --camera ID        id da camera (padrao: cam0)\n"
                 "  --window-seconds N  segundos de ring buffer (padrao: 30)\n"
                 "  --bitrate BPS     bitrate nominal usado para dimensionar o\n"
                 "                    ring buffer (padrao: 134400)\n";
}

// Quanto tempo o daemon espera a ingestao chegar num estado utilizavel antes
// de desistir. Em replay de arquivo isso e irrelevante (o arquivo inteiro cabe
// em centenas de milissegundos); numa camera real e o tempo que ela leva para
// encher a janela.
constexpr int kCaptureSettleTimeoutSeconds = 30;

// Quantas medicoes consecutivas precisam concordar para a ingestao ser
// considerada parada. Tres leituras com 100 ms de intervalo: um stream que
// parou e um stream que engasgou por mais de um quarto de segundo nao sao a
// mesma coisa, e esperar um unico zero seria correr.
constexpr int kStablePollsRequired = 3;
constexpr int kPollIntervalMs = 100;

// Espera o ring buffer ter material suficiente E parar de crescer.
//
// A segunda condicao nao e um detalhe. O replay de arquivo entrega os 10
// minutos de video em ~500 ms, entao o primeiro poll pode encontrar o buffer
// ainda enchendo, com o "instante mais recente" parado no meio da gravacao.
// O cenario ancoraria num ponto que muda de execucao para execucao e
// produziria um clipe diferente a cada rodada — o smoke test acusaria hash
// instavel sem que nada estivesse obviamente quebrado.
//
// Em camera ao vivo a ingestao nunca para, e o deadline e quem governa. Isso e
// o comportamento certo: numa camera o que importa e "ja tenho N segundos", nao
// "ja terminou".
bool wait_for_capture_to_settle(
    const std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade>& mediaBuffer,
    double minSpanSeconds
) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kCaptureSettleTimeoutSeconds);
    std::uint64_t previousIngested = 0;
    double previousSpan = -1.0;
    int stablePolls = 0;

    while (std::chrono::steady_clock::now() < deadline && g_stopRequested == 0) {
        const auto stats = mediaBuffer->bufferStats();
        if (stats.hasFrames && stats.spanSeconds() >= minSpanSeconds) {
            if (stats.framesIngestedTotal == previousIngested &&
                stats.spanSeconds() == previousSpan) {
                ++stablePolls;
            } else {
                stablePolls = 0;
            }
            previousIngested = stats.framesIngestedTotal;
            previousSpan = stats.spanSeconds();
            if (stablePolls >= kStablePollsRequired) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    return false;
}

// Extrai um clipe de demonstracao e imprime o descriptor.
//
// O instante do evento e escolhido DENTRO do intervalo que o ring buffer
// realmente guarda, e nao "agora". Isso nao e um detalhe: em replay de arquivo
// o video inteiro e consumido em centenas de milissegundos e o processo chega
// ao EOS, depois do qual o relogio de captura CONGELA. Um cenario ancorado no
// instante mais recente pediria um "depois do evento" que nunca existe, e a
// extracao ficaria esperando ate o timeout — foi exatamente assim que a suite
// E2E ficou flaky antes de o instante passar a ser derivado do buffer.
//
// Em camera real o relogio de captura continua avancando, e ancorar no buffer
// continua correto: a janela [T - pre, T] e exatamente o que se quer guardar
// em torno de um evento.
void run_demo_scenario(
    const std::shared_ptr<ods::s4::application::ExtractClipUseCase>& extractClip,
    const std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade>& mediaBuffer,
    const std::string& mediaDir,
    int port
) {
    using ods::s4::domain::kNanosecondsPerSecond;

    constexpr double kPreSeconds = 1.0;
    constexpr double kPostSeconds = 0.5;

    const double needed = kPreSeconds + kPostSeconds;
    std::cout << "Waiting for the ring buffer to hold at least " << needed
              << " s and stop growing...\n";
    std::cout.flush();

    if (!wait_for_capture_to_settle(mediaBuffer, needed)) {
        const auto stats = mediaBuffer->bufferStats();
        std::cerr << "ring buffer never settled holding " << needed << " s of video"
                  << " (has_frames=" << (stats.hasFrames ? "yes" : "no")
                  << ", span=" << stats.spanSeconds() << " s, ingested="
                  << stats.framesIngestedTotal << ", rejected-by-domain=see log)\n";
        return;
    }

    const auto stats = mediaBuffer->bufferStats();
    std::cout << "buffer: " << stats.framesStored << " frames, " << stats.bytesUsed
              << "/" << stats.capacityBytes << " bytes, span " << stats.spanSeconds()
              << " s, session " << stats.sessionId << '\n';
    std::cout.flush();

    // Fim da janela = instante mais recente guardado. Assim o "depois do
    // evento" ja chegou e a extracao nao precisa esperar.
    const auto eventTsNs = stats.newestCaptureTsNs -
        static_cast<ods::s4::domain::Nanoseconds>(kPostSeconds * kNanosecondsPerSecond);
    const auto window = ods::s4::domain::CaptureWindow::around(
        eventTsNs, kPreSeconds, kPostSeconds
    );

    const std::string outputPath =
        (std::filesystem::path(mediaDir) / "event-demo.mp4").string();
    try {
        const auto clip = extractClip->executeCaptureWindow(
            "event-demo", window, outputPath
        );
        std::cout << "clip_id: " << clip.clipId() << '\n';
        std::cout << "file:    " << clip.filePath() << '\n';
        std::cout << "sha256:  " << clip.sha256Hash() << '\n';
        std::cout << "GET http://127.0.0.1:" << port << "/api/v1/clips/"
                  << clip.clipId() << "\n";
        std::cout.flush();
    } catch (const std::exception& error) {
        std::cerr << "clip extraction failed: " << error.what() << '\n';
    }
}

} // namespace


int main(int argc, char* argv[]) {
    int port = 8080;
    std::string dbPath = "/tmp/s4_media_evidence.db";
    std::string mediaDir = "/tmp/s4_media_evidence";
    std::string sourcePath;
    std::string cameraId = "cam0";
    double windowSeconds = 30.0;
    long long bitrateBps = 134400;
    bool runScenario = false;

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
        } else if (read_value("--source")) {
            sourcePath = argv[i];
        } else if (read_value("--camera")) {
            cameraId = argv[i];
        } else if (read_value("--window-seconds")) {
            windowSeconds = std::stod(argv[i]);
        } else if (read_value("--bitrate")) {
            bitrateBps = std::stoll(argv[i]);
        } else if (argument == "--scenario") {
            runScenario = true;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!sourcePath.empty() && !std::filesystem::is_regular_file(sourcePath)) {
        std::cerr << "--source is not a readable file: '" << sourcePath << "'\n";
        return 1;
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
    // rodando sem camera; com --source, um MP4 real e replayado. Nada mais
    // nesta funcao muda: o ExtractClipUseCase so conhece a porta
    // IMediaBufferReader, entao trocar a fonte nao toca no resto do fluxo.
    ods::s4::application::RingBufferConfig bufferConfig;
    bufferConfig.cameraId = cameraId;
    bufferConfig.windowSeconds = windowSeconds;
    bufferConfig.bitrateBps = bitrateBps;
    bufferConfig.width = 320;
    bufferConfig.height = 240;
    // Daemon: apos uma queda, o /dev/shm da execucao anterior pode ter ficado
    // para tras e impediria a subida. Uma instancia por camera, entao limpar e
    // o comportamento correto aqui.
    bufferConfig.replaceStaleSegment = true;

    std::unique_ptr<ods::s4::domain::IFrameSource> frameSource;
    std::string sessionId;

    if (sourcePath.empty()) {
        ods::s4::infrastructure::SyntheticSourceOptions sourceOptions;
        sourceOptions.fps = 15.0;
        sourceOptions.gop = 15;
        sourceOptions.keyframeBytes = 6000;
        sourceOptions.deltaBytes = 1200;
        sourceOptions.sessionId = "exec-demo-1";
        sessionId = sourceOptions.sessionId;
        frameSource = std::make_unique<ods::s4::infrastructure::SyntheticFrameSource>(sourceOptions);
    } else {
#if ODS_S4_WITH_GSTREAMER
        // A fonte de replay e marcada com um id de sessao proprio: o ring
        // buffer invalida o conteudo anterior quando a sessao muda, e um id
        // estavel distingue "restartei a mesma fonte" de "comecei outra
        // gravacao". O id deriva do proprio arquivo, entao dois daemons do
        // mesmo video nao se confundem.
        sessionId = "replay-" +
            std::filesystem::path(sourcePath).filename().string();
        frameSource = std::make_unique<ods::s4::infrastructure::GStreamerAppsinkFrameSource>(
            ods::s4::infrastructure::GStreamerAppsinkFrameSource::fileReplayH264Pipeline(
                sourcePath
            ),
            sessionId
        );
#else
        std::cerr << "--source requires GStreamer; rebuild with -DODS_S4_WITH_GSTREAMER=ON\n";
        return 1;
#endif
    }

    std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade> mediaBuffer;
    try {
        mediaBuffer = std::make_shared<ods::s4::infrastructure::RingBufferRAMFacade>(
            std::move(bufferConfig), std::move(frameSource)
        );
    } catch (const ods::s4::domain::ODSBaseException& error) {
        std::cerr << "cannot start the S4.1 ring buffer: " << error.what() << std::endl;
        return 1;
    }
    auto fileStorage = std::make_shared<ods::s4::infrastructure::FileStorage>();

    // As duas portas do fluxo real (janela em relogio de captura + mux para MP4)
    // sao injetadas aqui, e nao dentro do caso de uso. Sem GStreamer elas ficam
    // nulas: o daemon continua servindo a API e executeCaptureWindow falha com
    // MediaMuxError, em vez de inventar um container.
    std::shared_ptr<ods::s4::application::IMediaMuxer> muxer;
#if ODS_S4_WITH_GSTREAMER
    muxer = std::make_shared<ods::s4::infrastructure::GStreamerMp4Muxer>();
#endif

    auto extractClip = std::make_shared<ods::s4::application::ExtractClipUseCase>(
        repository, mediaBuffer, fileStorage, mediaBuffer, muxer
    );

    mediaBuffer->startCapture(sessionId);

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
    if (!sourcePath.empty()) {
        std::cout << "Replaying '" << sourcePath << "' into the ring buffer\n";
    } else {
        std::cout << "Recording synthetic feed\n";
    }
    std::cout.flush();

    if (runScenario) {
        run_demo_scenario(extractClip, mediaBuffer, mediaDir, port);
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