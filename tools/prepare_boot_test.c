// Standalone harness for the on-console preparation: runs bb_prepare_boot against a dump folder
// and prints the result, so the C pipeline can be compared with scripts/prepare.py on a PC.
// Build (MSYS2 CLANG64 or clang-cl):
//   clang-cl /nologo /TC /std:c11 /DBB_PREPARE_STANDALONE prepare_boot.c prepare_boot_test.c
#include <stdio.h>

#include "prepare_boot.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dump folder> <output folder>\n", argv[0]);
        return 2;
    }
    char error[512] = {0};
    const int rc = bb_prepare_boot(argv[1], argv[2], error, sizeof(error));
    if (rc != 0) {
        fprintf(stderr, "prepare failed: %s\n", error);
        return 1;
    }
    return 0;
}
