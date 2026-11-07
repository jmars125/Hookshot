#include "../gpu/vk/movie_upscale_policy.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    using mkwvk::movie_upscale_policy::Mode;
    using mkwvk::movie_upscale_policy::parse;
    using mkwvk::movie_upscale_policy::use_linear;

    CHECK("unset defaults to automatic", parse(nullptr) == Mode::Automatic);
    CHECK("explicit auto uses automatic", parse("auto") == Mode::Automatic);
    CHECK("invalid values use automatic", parse("LINEAR") == Mode::Automatic && parse("plain") == Mode::Automatic);
    CHECK("linear override selects movie fallback before auto trigger",
          use_linear(parse("linear"), false, false, true, false));
    CHECK("default auto leaves movie networks enabled until existing trigger",
          !use_linear(parse(nullptr), false, false, true, false));
    CHECK("default auto follows existing fallback trigger",
          use_linear(parse(nullptr), true, false, true, false));
    CHECK("explicit keep-all preserves existing network override",
          !use_linear(parse("linear"), true, true, true, false));
    CHECK("gameplay networks are never changed", !use_linear(parse("linear"), true, false, true, true));
    CHECK("non-cooperative programs are unaffected", !use_linear(parse("linear"), true, false, false, false));
    std::puts("movie upscale policy: all checks passed");
}
