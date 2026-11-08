# SPIR-V global narrow load/store regression

This fixture builds a small `IR::Program`, runs the real shader-info pass and
`EmitSPIRV`, then checks both the emitted instructions and the generated module
with `spirv-val`. It covers signed and unsigned 8- and 16-bit loads at naturally
aligned offsets, preservation of high guest-address bits, and an access at byte
offset `0xffe` whose helper word base is `0xffc`. It also covers the narrow
stores: they insert into the aligned 32-bit word with a compare-exchange loop,
so two invocations writing different halves of one word do not lose each other's
value. A mesh-stage load and store cover the stage the #192 crash came from.
The existing zero-result behavior when the profile lacks 64-bit integer support
is checked too.

The test intentionally does not cover odd guest addresses, SSBO range behavior,
or real AMD hardware. It tests the SPIR-V backend's lowering contract only.
The game-free workflow checks out the pinned SPIR-V-Headers revision and uses
the system SPIR-V Tools validator.

Reproduce it with Clang in C++20 mode, as in the Linux workflow. The focused
target includes project compatibility headers with a function-local static
inside a `constexpr` assertion macro. GCC rejects that construct in C++20,
while the project's production Linux compiler is Clang in C++20 mode. The
fixture does not establish an unmodified MSVC build.
