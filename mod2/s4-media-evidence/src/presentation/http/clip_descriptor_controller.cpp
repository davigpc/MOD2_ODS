#include "s4/presentation/http/clip_descriptor_controller.hpp"

namespace ods::s4::presentation {

std::optional<application::ClipDescriptorDTO> ClipDescriptorController::getClipDescriptor(const std::string& clipId) const {
    if (!m_repository) {
        return std::nullopt;
    }

    auto clipOpt = m_repository->findById(clipId);
    if (!clipOpt.has_value()) {
        return std::nullopt;
    }

    const auto& clip = *clipOpt;
    application::ClipDescriptorDTO dto;
    dto.clipId = clip.clipId();
    dto.fileUri = clip.filePath();
    dto.sha256Hash = clip.sha256Hash();
    dto.isRetained = clip.isRetained();
    dto.isLockedForAudit = clip.isLockedForAudit();

    return dto;
}

} // namespace ods::s4::presentation
