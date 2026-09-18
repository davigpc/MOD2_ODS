#pragma once

#include <memory>
#include <string>
#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/repositories/media_clip_repository.hpp"
#include "s4/domain/value_objects/time_window.hpp"

namespace ods::s4::application {

class ExtractClipUseCase {
public:
    explicit ExtractClipUseCase(std::shared_ptr<domain::IMediaClipRepository> repository)
        : m_repository(std::move(repository)) {}

    domain::MediaClip execute(
        const std::string& eventId,
        const domain::TimeWindow& timeWindow,
        const std::string& destinationPath
    );

private:
    std::shared_ptr<domain::IMediaClipRepository> m_repository;
};

} // namespace ods::s4::application
