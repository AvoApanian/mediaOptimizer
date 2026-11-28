#include "../image_internal.hpp"

#include <jpeglib.h>

#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace mediaopt::web::detail {
namespace {

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

struct JpegState {
    jpeg_decompress_struct cinfo{};
    JpegError error{};

    unsigned char* rgba = nullptr;
    unsigned char* row = nullptr;

    bool created = false;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    auto* error = reinterpret_cast<JpegError*>(cinfo->err);

    error->base.format_message(cinfo, error->message);
    longjmp(error->jump, 1);
}

void jpeg_cleanup(JpegState* state) {
    if (!state) {
        return;
    }

    if (state->created) {
        jpeg_destroy_decompress(&state->cinfo);
        state->created = false;
    }

    std::free(state->rgba);
    std::free(state->row);

    state->rgba = nullptr;
    state->row = nullptr;

    delete state;
}

}

bool decode_jpeg(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
) {
    if (!input.valid()) {
        error = "JPEG input is empty";
        return false;
    }

    if (input.size > std::numeric_limits<unsigned long>::max()) {
        error = "JPEG input is too large";
        return false;
    }

    JpegState* state = new (std::nothrow) JpegState{};

    if (!state) {
        error = "Unable to allocate JPEG decoder state";
        return false;
    }

    state->cinfo.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_error_exit;

    if (setjmp(state->error.jump)) {
        error = state->error.message;
        jpeg_cleanup(state);
        return false;
    }

    state->created = true;
    jpeg_create_decompress(&state->cinfo);

    jpeg_mem_src(
        &state->cinfo,
        input.data,
        static_cast<unsigned long>(input.size)
    );

    if (jpeg_read_header(&state->cinfo, TRUE) != JPEG_HEADER_OK) {
        error = "Invalid JPEG header";
        jpeg_cleanup(state);
        return false;
    }

    state->cinfo.out_color_space = JCS_RGB;

    if (!jpeg_start_decompress(&state->cinfo)) {
        error = "Unable to start JPEG decompression";
        jpeg_cleanup(state);
        return false;
    }

    const std::uint64_t width = state->cinfo.output_width;
    const std::uint64_t height = state->cinfo.output_height;

    if (width == 0 || height == 0 ||
        width > UINT32_MAX || height > UINT32_MAX ||
        max_pixels == 0 ||
        width > max_pixels / height ||
        width > std::numeric_limits<std::size_t>::max() / 4 ||
        width > std::numeric_limits<std::size_t>::max() / 3) {
        error = "JPEG dimensions exceed supported limits";
        jpeg_cleanup(state);
        return false;
    }

    const std::uint64_t pixel_count = width * height;

    if (pixel_count > max_pixels ||
        pixel_count > std::numeric_limits<std::size_t>::max() / 4) {
        error = "JPEG image exceeds pixel limit";
        jpeg_cleanup(state);
        return false;
    }

    const std::size_t rgba_size =
        static_cast<std::size_t>(pixel_count) * 4;

    const std::size_t row_size =
        static_cast<std::size_t>(width) * 3;

    state->rgba = static_cast<unsigned char*>(std::malloc(rgba_size));
    state->row = static_cast<unsigned char*>(std::malloc(row_size));

    if (!state->rgba || !state->row) {
        error = "Unable to allocate JPEG pixel buffers";
        jpeg_cleanup(state);
        return false;
    }

    while (state->cinfo.output_scanline < state->cinfo.output_height) {
        JSAMPROW row_pointer = state->row;

        if (jpeg_read_scanlines(
                &state->cinfo,
                &row_pointer,
                1
            ) != 1) {
            error = "Unable to read JPEG scanline";
            jpeg_cleanup(state);
            return false;
        }

        const std::size_t y = state->cinfo.output_scanline - 1;

        unsigned char* destination =
            state->rgba + y * static_cast<std::size_t>(width) * 4;

        const unsigned char* source = state->row;

        for (std::size_t x = 0; x < static_cast<std::size_t>(width); ++x) {
            destination[x * 4] = source[x * 3];
            destination[x * 4 + 1] = source[x * 3 + 1];
            destination[x * 4 + 2] = source[x * 3 + 2];
            destination[x * 4 + 3] = 255;
        }
    }

    if (!jpeg_finish_decompress(&state->cinfo)) {
        error = "Unable to finish JPEG decompression";
        jpeg_cleanup(state);
        return false;
    }

    try {
	    DecodedImage decoded;

    	decoded.width = static_cast<std::uint32_t>(width);
    	decoded.height = static_cast<std::uint32_t>(height);
    		decoded.pixels.assign(
        	state->rgba,
        		state->rgba + rgba_size
		    );
	
    	jpeg_cleanup(state);
    	state = nullptr;

    	output = std::move(decoded);
	} catch (const std::bad_alloc&) {
	    jpeg_cleanup(state);
	    error = "Unable to allocate decoded JPEG image";
    	return false;
	}

	return true;

}

}
