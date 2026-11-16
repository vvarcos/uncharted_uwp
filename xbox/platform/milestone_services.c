/* M1 UWP link adapters. Renderer/audio/input integration follows in later milestones. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>

#define UNPORTED(name) uintptr_t runtime_##name##_resolve(const char *nid) { (void)nid; return 0; }

UNPORTED(ajm)
UNPORTED(pad)
void runtime_ajm_report(void) { puts("UWP milestone: media decoding deferred"); }
void runtime_pad_report(void) { puts("UWP milestone: controller API deferred"); }
int runtime_pad_describe(void) { return 0; }
int runtime_pad_tool(int test, int strength) { (void)test; (void)strength; return 0; }
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) { if (count) *count=0; return 0; }

/* M4 registers the unmodified shadPS4 event queues through a standalone UWP-safe
 * archive member. Renderer/media stubs remain until their own contracts are ported. */
#ifdef UNCHARTED_BUILD_GUEST
extern void bbgpu_uwp_kernel_register(void);
extern uintptr_t bbgpu_uwp_kernel_resolve(const char *nid);
extern unsigned bbgpu_uwp_kernel_symbol_count(void);
void bbgpu_register_kernel(void) {
    bbgpu_uwp_kernel_register();
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
    printf("M5: HLE registered: %u symbols (17 real equeue + 6 logical VideoOut + 2 GNM IRQ; renderer absent)\n",
           bbgpu_uwp_kernel_symbol_count());
#else
    printf("M4: real kernel equeue contracts registered: %u symbols\n",
           bbgpu_uwp_kernel_symbol_count());
#endif
}
uintptr_t bbgpu_resolve(const char *nid) { return bbgpu_uwp_kernel_resolve(nid); }
unsigned bbgpu_symbol_count(void) { return bbgpu_uwp_kernel_symbol_count(); }
#else
/* The executable links bbruntime without the guest/renderer sources, so this file is compiled
 * without UNCHARTED_BUILD_GUEST and only the milestone stubs are available. The renderer DLL links
 * the real implementations from platform/milestone_services_guest.c, which must override these —
 * hence the weak linkage. Without that override bbgpu_resolve() returned 0 for every import and
 * the guest died on the first one (sceKernelCreateEqueue, NID D0OdFMjp46I), while the only trace
 * was a "GPU event queues deferred" line. These stubs are therefore fail-loud, and probe.c aborts
 * with a named reason when the self-test finds an empty table. */
__attribute__((weak)) void bbgpu_register_kernel(void) {
    fprintf(stderr, "ERROR: guest HLE is not linked in this target "
                    "(bbruntime built without UNCHARTED_BUILD_GUEST)\n");
}
__attribute__((weak)) uintptr_t bbgpu_resolve(const char *nid) {
    fprintf(stderr, "ERROR: bbgpu_resolve('%s') on a target without the guest HLE\n",
            nid ? nid : "?");
    return 0;
}
__attribute__((weak)) unsigned bbgpu_symbol_count(void) { return 0; }
/* GNM activity counters live with the driver in the renderer DLL; the executable has no driver. */
__attribute__((weak)) unsigned bbgpu_gnm_stats(char *out, unsigned capacity) {
    (void)out; (void)capacity; return 0;
}
#endif
int bbgpu_init(const BbGpuConfig *config) { (void)config; return -1; }
int bbgpu_handle_fault(void *context, void *address) { (void)context; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *context) { (void)context; }
int bbgpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int bbgpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 2; }
int bbgpu_overlay_captures_input(void) { return 0; }
void bbgpu_prompts_controller(int device) { (void)device; }
void bbgpu_mouse_configure(int look) { (void)look; }
int bbgpu_camera_yaw(float *yaw_degrees, double *age_seconds) {
    if (yaw_degrees) *yaw_degrees = 0; if (age_seconds) *age_seconds = 0; return 0;
}
int bbgpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel) {
    if (dx) *dx = 0; if (dy) *dy = 0; if (buttons) *buttons = 0; if (wheel) *wheel = 0; return 0;
}
void bbgpu_trophy_notify(int grade, const char *game, const char *name, const void *png, uint64_t size) {
    (void)grade; (void)game; (void)name; (void)png; (void)size;
}
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *version) {
    (void)app0; (void)serial; (void)title; (void)version;
}
void bbnet_register(void) { puts("UWP M2: co-op networking disabled"); }

int vulkan_smoke(void) { puts("UWP M2: use the CoreWindow graphics probe"); return 21; }
