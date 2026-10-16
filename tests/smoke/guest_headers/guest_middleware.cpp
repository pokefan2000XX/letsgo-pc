// Compile-only check for the middleware headers (nn::gfx, NintendoWare ui2d,
// g3d, vfx, atk, font). These libraries are statically linked inside main, so
// decompiling code that uses them needs their declarations.
#include <nn/atk.h>
#include <nn/font/font_Util.h>
#include <nn/g3d/g3d_ResFile.h>
#include <nn/gfx/gfx_Device.h>
#include <nn/ui2d/ui2d_Layout.h>
#include <nn/vfx/vfx_System.h>

extern "C" int guest_middleware_probe() { return 0; }
