/* Drawing: vertex fetch -> vertex shader -> primitive assembly ->
 * rasterisation -> colour combiners -> framebuffer. */
#include <inttypes.h>

#include "pica.h"

void pica_draw(int indexed)
{
    gpu_fatal("%s of %" PRIu32 " vertices: drawing is not implemented yet",
              indexed ? "DRAWELEMENTS" : "DRAWARRAYS", pica.reg[REG_NUMVERTICES]);
}

void pica_immediate_vertex(const Vec4 *attribs)
{
    (void)attribs;
    gpu_fatal("immediate-mode vertices: drawing is not implemented yet");
}

void pica_restart_primitive(void)
{
}
