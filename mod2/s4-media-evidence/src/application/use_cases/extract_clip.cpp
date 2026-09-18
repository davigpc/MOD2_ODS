#include "s4/application/use_cases/extract_clip.hpp"

#include "s4/infrastructure/hashing/sha256_hasher.hpp"

#include <uuid/uuid.h>

#include <cstdint>
#include <vector>

namespace ods::s4::application {

namespace {

std::string generate_clip_id() {
    uuid_t uuid;
    char buffer[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, buffer);
    return std::string("clip-") + buffer;
}

} // namespace

ExtractClipUseCase::ExtractClipUseCase(
    std::shared_ptr<domain::IMediaClipRepository> repository,
    std::shared_ptr<domain::IMediaBufferReader> buffer,
    std::shared_ptr<domain::IFileStorage> fileStorage
) : m_repository(std::move(repository)),
    m_buffer(std::move(buffer)),
    m_fileStorage(std::move(fileStorage)) {}

domain::MediaClip ExtractClipUseCase::execute(
    const std::string& eventId,
    const domain::TimeWindow& timeWindow,
    const std::string& destinationPath
) {
    const auto frames = m_buffer->readWindow(timeWindow.startTime(), timeWindow.endTime());
    if (frames.empty()) {
        throw domain::MediaBufferEmptyError("no frames available in the requested time window");
    }

    std::vector<std::uint8_t> payload;
    std::size_t totalSize = 0;
    for (const auto& frame : frames) {
        totalSize += frame.size();
    }
    payload.reserve(totalSize);
    for (const auto& frame : frames) {
        payload.insert(payload.end(), frame.begin(), frame.end());
    }

    m_fileStorage->write(destinationPath, payload);
    const std::string sha256Hash = infrastructure::compute_sha256_hex(payload);

    domain::MediaClip clip(generate_clip_id(), eventId, destinationPath, sha256Hash, timeWindow);
    m_repository->save(clip);
    return clip;
}

} // namespace ods::s4::application