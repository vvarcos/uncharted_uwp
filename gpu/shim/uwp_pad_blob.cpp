// UWP M5 experiment: inert data used to test whether package activation depends on image size.
// Compiled only when UNCHARTED_PAD_BYTES is defined by the build.
#ifdef UNCHARTED_PAD_BYTES
#include <cstddef>

extern "C" const unsigned char bb_pad_blob[UNCHARTED_PAD_BYTES] = {0xA5};

extern "C" int bb_pad_touch() {
    static volatile unsigned char sink;
    sink = bb_pad_blob[0];
    return sink;
}
#endif

#ifdef UNCHARTED_TLS_PAD_BYTES
// TLS-limit experiment: static thread-local bytes of the same order the renderer objects add.
thread_local unsigned char bb_tls_pad[UNCHARTED_TLS_PAD_BYTES];

extern "C" int bb_tls_touch() {
    bb_tls_pad[0] = 0x5A;
    return bb_tls_pad[0];
}
#endif
