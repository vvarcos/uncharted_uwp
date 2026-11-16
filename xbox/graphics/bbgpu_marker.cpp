// This object lives in the bbgpu archive. Referencing it from the host proves the UWP renderer
// target was compiled and linked without dragging desktop-only renderer entry points into M2.
extern "C" int bbgpu_uwp_archive_marker() { return 0xBB02; }
