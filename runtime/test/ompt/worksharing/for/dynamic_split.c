// RUN: %libomp-compile-and-run | %sort-threads | %filecheck %S/base_split.h
// REQUIRES: ompt

#define SCHEDULE dynamic
#include "base_split.h"