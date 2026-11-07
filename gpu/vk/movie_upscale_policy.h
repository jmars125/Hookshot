/* Movie-only linear-upscale override policy. */
#pragma once

#include <cstring>

namespace mkwvk::movie_upscale_policy {

enum class Mode { Automatic, Linear };

inline Mode parse(const char* value) {
    return value && std::strcmp(value, "linear") == 0 ? Mode::Linear : Mode::Automatic;
}

inline bool use_linear(Mode mode, bool automatic_fallback, bool keep_all, bool cooperative_matrix, bool gameplay_network) {
    return cooperative_matrix && !gameplay_network && !keep_all && (mode == Mode::Linear || automatic_fallback);
}

} // namespace mkwvk::movie_upscale_policy
