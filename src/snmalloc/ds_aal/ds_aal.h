#pragma once
/**
 * The early data-structures for snmalloc.  These provide some basic helpers that do
 * not depend on anything except for a working C++ implementation and the Aal.
 *
 * Files in this directory may not include anything from any other directory in
 * snmalloc.
 */

#include "flaglock.h"
#include "singleton.h"