/* SPDX-License-Identifier: GPL-2.0-or-later
 * On-console preparation: build the BBPROBE5 image the loader expects directly from the dump,
 * so installing the package and pointing it at E:\ is enough. Port of scripts/prepare.py and
 * scripts/link_modules.py; the emitted bytes are meant to be identical to the Python pipeline.
 *
 * Layouts (little endian, no host struct packing):
 *   BBPROBE5 = header(8s + 6Q) | procparam | main TLS (4Q) | module count + 7Q each |
 *              binding count + 3Q each | segments | 128-byte names | relocations | image
 */
#ifndef BBPORT_PREPARE_BOOT_H
#define BBPORT_PREPARE_BOOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads eboot.bin, sce_module/libc.prx and libSceFios2.prx from `dump`, writes boot-linked.bin
 * and content.bin into `out_dir`. Returns 0 on success; on failure returns -1 and fills `err`. */
int bb_prepare_boot(const char* dump, const char* out_dir, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

#endif
