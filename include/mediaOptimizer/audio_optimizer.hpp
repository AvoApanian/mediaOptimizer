#pragma once

#include "media_optimizer.hpp"

namespace mediaopt {

[[nodiscard]] Result optimize_audio(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const AudioConfig& config = {}
);

}
