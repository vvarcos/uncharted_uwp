#pragma once
#include "bb_win_compat.h"
#include "runtime_compat.h"
#include <pthread.h>
#include <io.h>
#ifndef ssize_t
typedef intptr_t ssize_t;
#endif
#define fseeko _fseeki64
#define ftello _ftelli64
#define fileno _fileno

