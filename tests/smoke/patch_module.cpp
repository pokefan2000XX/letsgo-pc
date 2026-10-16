// Smoke test for the decomp side: C++ compiled for the Switch's CPU (AArch64,
// Itanium C++ ABI, LP64 - the same rules the original was built with), linked
// at a fixed address next to main and calling an original game function by
// its address. Decompiled functions are built exactly like this, then the
// recompiler translates them together with the untouched original code.
//
// Build:
//   clang++ --target=aarch64-none-elf -O2 -std=c++20 -fno-exceptions -fno-rtti -ffreestanding \
//           -nostdlib -nostdinc++ -Wno-invalid-offsetof -fno-pic -mno-outline-atomics -c tests/smoke/patch_module.cpp -o build/smoke/patch_module.o
//   ld.lld -T tests/smoke/patch_module.ld build/smoke/patch_module.o -o build/smoke/patch_module.elf
// No standard headers: guest-side libc/libc++ headers are set up later for the real decomp build.
#define offsetof(t, m) __builtin_offsetof(t, m)

extern "C" void nnMain();  // original game function at 0x7100004110 (defined in the linker script)

// Layout checks: these must hold for decomp code to share objects with the original.
struct Base {
    virtual ~Base() {}
    virtual int Get() const { return 0; }
    long value;
};
struct Derived : Base {
    int Get() const override { return extra; }
    int extra;
};
static_assert(sizeof(long) == 8, "guest is LP64");
static_assert(sizeof(void*) == 8, "64-bit pointers");
static_assert(offsetof(Derived, value) == 8 && offsetof(Derived, extra) == 16, "Itanium layout: vptr first");
static_assert(sizeof(Derived) == 24, "Itanium tail layout");

extern "C" int patch_entry(const Base* b) {
    nnMain();
    return b->Get();  // virtual call through the vtable, exactly as the original code does it
}
