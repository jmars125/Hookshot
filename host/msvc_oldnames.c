/* (hookshot, issue 439) oldnames.lib is the aliases from the C runtime's pre-2015 (non-underscore) names to its current ones.
   clang asks for it as a default library for the MSVC target, but recent Windows SDKs no longer ship it (an older SDK has it
   in ucrt\x64; Visual Studio has its own copy in its lib folder), so a link whose library paths reach neither failed after
   the whole game had been compiled. toolchain.native_link() no longer asks for it, and this file holds the aliases the
   prebuilt static FFmpeg libraries (the CPM cache, built with Microsoft's compiler) still need: avutil's log.o isatty, its
   file.o close, and avcodec's/avutil's file_open.o fdopen and tempnam. The MinGW and POSIX builds compile nothing of it,
   their C library has these names itself. */
#if defined(_MSC_VER) && !defined(__MINGW32__)
#pragma comment(linker, "/alternatename:close=_close")
#pragma comment(linker, "/alternatename:fdopen=_fdopen")
#pragma comment(linker, "/alternatename:isatty=_isatty")
#pragma comment(linker, "/alternatename:tempnam=_tempnam")
#else
typedef int msvc_oldnames_unused;   /* (a translation unit with something in it) */
#endif
