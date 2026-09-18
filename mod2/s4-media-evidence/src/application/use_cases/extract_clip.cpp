#include "s4/application/use_cases/extract_clip.hpp"
#include <uuid/uuid.h>
#include <sstream>
#include <iomanip>

namespace ods::s4::application {

domain::MediaClip ExtractClipUseCase::execute(
    const std::string& eventId,
    const domain::TimeWindow& timeWindow,
    const std::string& destinationPath
) {
    // Generate pseudo/mock clip ID or use a UUID implementation
    std::string clipId = "clip-" + eventId;
    std::string sha256Placeholder = "dummy_sha256_hash";

    domain::MediaClip clip(
        clipId,
        eventId,
        destinationPath,
        sha256Placeholder,
        timeWindow
    );

    if (m_repository) {
        m_repository->save(clip);
    }

    return clip;
}

} // namespace ods::s4::application
