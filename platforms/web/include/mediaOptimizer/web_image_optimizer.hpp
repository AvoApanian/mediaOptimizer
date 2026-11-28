#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mediaopt::web {

enum class Status : std::uint8_t {
	Optimized,
	OriginalKept,
	Error
};

enum class Format : std::uint8_t {
	Unknown,
	JPEG,
	PNG,
	WebP
};

struct ImageView {
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;

	bool valid() const noexcept {
		return data != nullptr && size > 0;
	}
};

struct ImageConfig {
	std::uint32_t max_dimension = 2560;
	int webp_quality = 80;
	double min_psnr_db = 22.0;

	std::size_t max_input_bytes = 128ULL * 1024ULL * 1024ULL;
	std::uint64_t max_pixels = 100000000ULL;
};

struct ImageResult {
	Status status = Status::Error;

	Format input_format = Format::Unknown;
	Format output_format = Format::Unknown;

	std::vector<std::uint8_t> output;

	std::size_t input_size = 0;

	std::uint32_t input_width = 0;
	std::uint32_t input_height = 0;

	std::uint32_t output_width = 0;
	std::uint32_t output_height = 0;

	std::string reason;
	std::string error;

	bool success() const noexcept {
		return status != Status::Error;
	}

	bool was_optimized() const noexcept {
		return status == Status::Optimized;
	}

	bool kept_original() const noexcept {
		return status == Status::OriginalKept;
	}

	std::size_t output_size() const noexcept {
		return output.size();
	}
};

[[nodiscard]] ImageResult optimize_image(
	ImageView input,
	const ImageConfig& config = {}
);

}
