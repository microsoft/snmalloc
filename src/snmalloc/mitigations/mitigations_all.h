#pragma once
/**
 * The mitigations layer provides compile-time configuration for security
 * mitigations. It sits between ds_aal/ and pal/ in the include hierarchy.
 *
 * Files in this directory may include ds_core/ and ds_aal/ but not pal/
 * or anything above.
 */

#include "allocconfig.h"
#include "cheri.h"
#include "mitigations.h"
