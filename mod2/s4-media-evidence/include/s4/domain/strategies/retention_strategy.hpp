#pragma once

#include <vector>
#include "s4/domain/entities/media_clip.hpp"

namespace ods::s4::domain {

class IRetentionStrategy {
public:
    virtual ~IRetentionStrategy() = default;

    [[nodiscard]] virtual std::vector<MediaClip> selectClipsToPurge(
        const std::vector<MediaClip>& candidates
    ) const = 0;
};

} // namespace ods::s4::domain
