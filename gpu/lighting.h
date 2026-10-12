/* Fragment lighting (lighting.c). */
#ifndef GPU_LIGHTING_H
#define GPU_LIGHTING_H

#include "gpu.h"

/* Light one fragment: its interpolated quaternion (x, y, z, w) and view
   vector, and the bump map's texel if bump mapping is on (else NULL).
   Gives the fragment primary and secondary colours the combiners read. */
void lighting_compute(const float quat[4], const float view[3], const Rgba *bump, Rgba *primary, Rgba *secondary);

#endif
