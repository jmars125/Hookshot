# SPIR-V global halfword load regression

This fixture builds a small `IR::Program`, runs the real shader-info pass and
`EmitSPIRV`, then checks both the emitted instructions and the generated module
with `spirv-val`. It covers signed and unsigned loads at naturally halfword-
aligned offsets 0 and 2, preservation of high guest-address bits, and an access
at byte offset `0xffe` whose helper word base is `0xffc`. It also covers the
existing zero-result behavior when the profile lacks 64-bit integer support.

The test intentionally does not cover odd guest addresses, SSBO range behavior,
or real AMD hardware. It tests the SPIR-V backend's lowering contract only.
The game-free workflow checks out the pinned SPIR-V-Headers revision and uses
the system SPIR-V Tools validator.

Reproduce it with Clang in C++20 mode, as in the Linux workflow. The focused
target includes project compatibility headers with a function-local static
inside a `constexpr` assertion macro. GCC rejects that construct in C++20,
while the project's production Linux compiler is Clang in C++20 mode. The
fixture does not establish an unmodified MSVC build.
