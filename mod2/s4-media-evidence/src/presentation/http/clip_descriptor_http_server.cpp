#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#include "s4/application/dtos/clip_descriptor_dto.hpp"
#include "s4/presentation/http/clip_descriptor_controller.hpp"

#include "httplib.h"

#include <uuid/uuid.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <thread>

namespace ods::s4::presentation {

namespace {

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
                break;
        }
    }
    return out;
}

std::string format_double(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

std::string generate_event_id() {
    uuid_t uuid;
    char buffer[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, buffer);
    return std::string("event-") + buffer;
}

bool is_valid_event_id(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

std::string extract_json_string(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) {
        return {};
    }
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) {
        return {};
    }
    const std::size_t quote1 = body.find('"', colon + 1);
    if (quote1 == std::string::npos) {
        return {};
    }
    const std::size_t quote2 = body.find('"', quote1 + 1);
    if (quote2 == std::string::npos) {
        return {};
    }
    return body.substr(quote1 + 1, quote2 - quote1 - 1);
}

std::optional<long long> extract_json_int(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t valueBegin = body.find_first_not_of(" \t\r\n", colon + 1);
    if (valueBegin == std::string::npos) {
        return std::nullopt;
    }
    std::size_t end = valueBegin;
    while (end < body.size() &&
           (std::isdigit(static_cast<unsigned char>(body[end])) || body[end] == '-')) {
        ++end;
    }
    if (end == valueBegin) {
        return std::nullopt;
    }
    try {
        return std::stoll(body.substr(valueBegin, end - valueBegin));
    } catch (...) {
        return std::nullopt;
    }
}

std::string to_json(const application::ClipDescriptorDTO& dto) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"clip_id\": \"" << json_escape(dto.clipId) << "\",\n";
    out << "  \"file_uri\": \"" << json_escape(dto.fileUri) << "\",\n";
    out << "  \"start_time\": \"" << json_escape(dto.startTimeIso) << "\",\n";
    out << "  \"end_time\": \"" << json_escape(dto.endTimeIso) << "\",\n";
    out << "  \"points_of_interest\": [";
    for (std::size_t i = 0; i < dto.pointsOfInterest.size(); ++i) {
        const auto& point = dto.pointsOfInterest[i];
        if (i > 0) {
            out << ',';
        }
        out << "\n    {";
        out << "\"x\": " << format_double(point.x);
        out << ", \"y\": " << format_double(point.y);
        out << ", \"label\": \"" << json_escape(point.label) << "\"";
        out << "}";
    }
    if (!dto.pointsOfInterest.empty()) {
        out << '\n';
    }
    out << "],\n";
    out << "  \"sha256_hash\": \"" << json_escape(dto.sha256Hash) << "\",\n";
    out << "  \"is_retained\": " << (dto.isRetained ? "true" : "false") << ",\n";
    out << "  \"is_locked_for_audit\": " << (dto.isLockedForAudit ? "true" : "false") << "\n";
    out << "}";
    return out.str();
}

} // namespace

struct ClipDescriptorHttpServer::Impl {
    std::shared_ptr<domain::IMediaClipRepository> repository;
    std::shared_ptr<ClipDescriptorController> controller;
    std::shared_ptr<application::ExtractClipUseCase> clipUseCase;
    std::string mediaDir;
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> running{false};
    int boundPort{0};
};

ClipDescriptorHttpServer::ClipDescriptorHttpServer(
    std::shared_ptr<domain::IMediaClipRepository> repository,
    std::shared_ptr<application::ExtractClipUseCase> clipUseCase,
    std::string mediaDir
) : m_impl(std::make_shared<Impl>()) {
    m_impl->repository = std::move(repository);
    m_impl->clipUseCase = std::move(clipUseCase);
    m_impl->mediaDir = std::move(mediaDir);
    m_impl->controller = std::make_shared<ClipDescriptorController>(m_impl->repository);

    m_impl->server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
        response.status = 200;
        response.set_content(R"({"status": "ok"})", "application/json");
    });

    m_impl->server.Get(R"(/api/v1/clips/[A-Za-z0-9_-]+)",
        [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const auto& path = request.path;
                const std::size_t clipIdBegin = path.rfind('/');
                const std::string clipId =
                    clipIdBegin == std::string::npos ? std::string{} : path.substr(clipIdBegin + 1);

                const auto descriptor = m_impl->controller->getClipDescriptor(clipId);
                if (!descriptor.has_value()) {
                    response.status = 404;
                    response.set_content(R"({"error":"clip not found"})", "application/json");
                    return;
                }
                response.status = 200;
                response.set_content(to_json(*descriptor), "application/json; charset=utf-8");
            } catch (const std::exception& error) {
                response.status = 500;
                response.set_content(
                    "{\"error\":\"" + json_escape(error.what()) + "\"}",
                    "application/json"
                );
            }
        });

    if (m_impl->clipUseCase) {
        m_impl->server.Post("/api/v1/events",
            [this](const httplib::Request& request, httplib::Response& response) {
                try {
                    const std::string& body = request.body;

                    std::string eventId = extract_json_string(body, "event_id");
                    if (eventId.empty()) {
                        eventId = generate_event_id();
                    }
                    if (!is_valid_event_id(eventId)) {
                        response.status = 400;
                        response.set_content(R"({"error":"invalid event_id"})", "application/json");
                        return;
                    }

                    using Clock = std::chrono::system_clock;
                    auto start = Clock::now() - std::chrono::seconds(1);
                    auto end = Clock::now();
                    if (const auto startMs = extract_json_int(body, "start_ms"); startMs.has_value()) {
                        start = Clock::time_point(std::chrono::milliseconds(*startMs));
                    }
                    if (const auto endMs = extract_json_int(body, "end_ms"); endMs.has_value()) {
                        end = Clock::time_point(std::chrono::milliseconds(*endMs));
                    }
                    if (start >= end) {
                        response.status = 400;
                        response.set_content(
                            R"({"error":"start_ms must be earlier than end_ms"})",
                            "application/json"
                        );
                        return;
                    }

                    const domain::TimeWindow timeWindow(start, end);
                    const std::string outputPath =
                        (std::filesystem::path(m_impl->mediaDir) / (eventId + ".mp4")).string();
                    const auto clip = m_impl->clipUseCase->execute(eventId, timeWindow, outputPath);
                    const auto descriptor = m_impl->controller->getClipDescriptor(clip.clipId());
                    response.status = 201;
                    response.set_content(to_json(*descriptor), "application/json; charset=utf-8");
                } catch (const std::exception& error) {
                    response.status = 500;
                    response.set_content(
                        "{\"error\":\"" + json_escape(error.what()) + "\"}",
                        "application/json"
                    );
                }
            });
    }
}

ClipDescriptorHttpServer::~ClipDescriptorHttpServer() {
    stop();
}

bool ClipDescriptorHttpServer::bind(int port) {
    if (port == 0) {
        m_impl->boundPort = m_impl->server.bind_to_any_port("0.0.0.0");
        return m_impl->boundPort > 0;
    }
    m_impl->boundPort = port;
    return m_impl->server.bind_to_port("0.0.0.0", port);
}

bool ClipDescriptorHttpServer::start() {
    if (m_impl->running.load()) {
        return true;
    }
    if (m_impl->boundPort <= 0) {
        return false;
    }
    m_impl->running.store(true);
    m_impl->thread = std::thread([this] {
        m_impl->server.listen_after_bind();
    });
    return true;
}

void ClipDescriptorHttpServer::stop() {
    if (!m_impl->running.exchange(false)) {
        return;
    }
    m_impl->server.stop();
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
}

int ClipDescriptorHttpServer::port() const {
    return m_impl->boundPort;
}

bool ClipDescriptorHttpServer::running() const {
    return m_impl->running.load();
}

} // namespace ods::s4::presentation