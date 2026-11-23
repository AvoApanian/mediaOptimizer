#pragma once

#include "media_optimizer.hpp"

namespace mediaopt {

[[nodiscard]] Result optimize_image(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const ImageConfig& config = {}
);

} // namespace mediaopt
