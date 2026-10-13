/* The CRO modules linked into the M13 test (recompiled by recomp --cro). */
#include "context.h"

extern const RecompModule recomp_module_mod_a, recomp_module_mod_b;

const RecompModule *const *recomp_module_table(void)
{
    static const RecompModule *const modules[] = { &recomp_module_mod_a, &recomp_module_mod_b, NULL };
    return modules;
}
