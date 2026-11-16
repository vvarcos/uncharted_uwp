#include <cstdio>

extern "C" int bbgpu_uwp_archive_marker();
extern "C" int uncharted_bbgpu_link_smoke() {
    const int marker = bbgpu_uwp_archive_marker();
    std::printf("M2 bbgpu archive linked: marker=0x%x\n", marker);
    return marker == 0xBB02 ? 0 : 1;
}
