// Maxwell (SM 5.x) machine code -> GLSL (Vulkan) translator.
//
// Some NVN programs ship without NV_gpu_program5 assembly in their control section
// (NintendoWare's built-in ui2d and font shaders, which draw every layout and all text),
// so for those the GPU code itself is translated. The output follows nvasm's resource
// layout (see nvasm.h), so the Vulkan backend binds both kinds of program the same way.
//
// NVN data section ("code"): a 0x30-byte header, the 0x50-byte shader program header
// (SPH) at control+0x708, then instructions in groups of four 64-bit words (a scheduling
// word, then three instructions), and the compiler's constants at control+0x700
// (control+0x6FC bytes), which the hardware sees as constant buffer 1.
//
// Hardware constant buffer slots of an NVN program and where they go:
//   0      driver data (storage buffer N: 64-bit address at 0x110 + 16N) -> buffer 15
//   1      the compiler's constants                                       -> inlined
//   2      bound texture handles (binding N at byte 0x20 + 8N)            -> buffer 14
//   3 + N  uniform buffer N                                               -> buffer N
#pragma once
#include <cstddef>
#include <cstdint>

#include "nvasm.h"

namespace maxwell {

// `control` is NVNshaderData::control, `code` the program's data section; the sizes bound
// every read. Options::uint_inputs / sint_inputs give the vertex attributes whose format
// is an unsigned / signed integer one (the GPU code reads raw attribute bits either way).
nvasm::Result translate(const uint8_t* control, size_t control_size, const uint8_t* code, size_t code_size,
                        const nvasm::Options& opt = {});

// bytes of the data section the program uses (to copy it), 0 if `control` is not understood
size_t data_size(const uint8_t* control, size_t control_size);

}  // namespace maxwell
