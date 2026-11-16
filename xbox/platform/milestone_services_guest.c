/* Renderer-DLL side of the kernel HLE bridge.
 *
 * bbruntime is a static library shared by the thin executable and the renderer DLL, and it is
 * compiled without UNCHARTED_BUILD_GUEST (the executable must not reference the renderer). Its
 * milestone_services.c therefore provides weak stubs. This translation unit is compiled only into
 * bbgpu_dll and provides the strong definitions, so the guest's imports are resolved against the
 * real HLE symbol table.
 *
 * Before this existed, bbgpu_resolve() always returned 0: every PS4 import fell through to the
 * probe's trap and the guest stopped at the first one (sceKernelCreateEqueue, NID D0OdFMjp46I),
 * printing "Original guest entry instructions executed; game initialization is incomplete."
 */
#include "runtime.h"
#include "gpu/bbgpu.h"

#include <stdio.h>

extern void bbgpu_uwp_kernel_register(void);
extern uintptr_t bbgpu_uwp_kernel_resolve(const char* nid);
extern unsigned bbgpu_uwp_kernel_symbol_count(void);

void bbgpu_register_kernel(void) {
    bbgpu_uwp_kernel_register();
    printf("M5: HLE registered: %u symbols\n", bbgpu_uwp_kernel_symbol_count());
}

uintptr_t bbgpu_resolve(const char* nid) {
    return bbgpu_uwp_kernel_resolve(nid);
}

unsigned bbgpu_symbol_count(void) {
    return bbgpu_uwp_kernel_symbol_count();
}

/* Strong definition: the driver's own counters (gnmdriver.cpp, renderer DLL). */
extern unsigned bbgpu_gnm_stats_impl(char* out, unsigned capacity);

unsigned bbgpu_gnm_stats(char* out, unsigned capacity) {
    return bbgpu_gnm_stats_impl(out, capacity);
}
