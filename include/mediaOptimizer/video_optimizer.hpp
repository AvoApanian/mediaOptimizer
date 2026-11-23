#pragma once

#include "media_optimizer.hpp"

namespace mediaopt {

[[nodiscard]] Result optimize_video(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const VideoConfig& config = {}
);

} // namespace mediaopt
