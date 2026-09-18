#pragma once

#include <memory>
#include <string>

#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/repositories/file_storage.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"
#include "s4/domain/services/media_buffer_reader.hpp"
#include "s4/domain/value_objects/time_window.hpp"

namespace ods::s4::application {

class ExtractClipUseCase {
public:
    ExtractClipUseCase(
        std::shared_ptr<domain::IMediaClipRepository> repository,
        std::shared_ptr<domain::IMediaBufferReader> buffer,
        std::shared_ptr<domain::IFileStorage> fileStorage
    );

    domain::MediaClip execute(
        const std::string& eventId,
        const domain::TimeWindow& timeWindow,
        const std::string& destinationPath
    );

private:
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
    std::shared_ptr<domain::IMediaBufferReader> m_buffer;
    std::shared_ptr<domain::IFileStorage> m_fileStorage;
};

} // namespace ods::s4::application