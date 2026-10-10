/* Negative test: `qadd` (saturating add, an ARMv5TE DSP instruction) isn't
   translated yet. The recompiler must refuse it with the address and
   mnemonic. (This used to be `mul`, then `umull`, until those were added.) */
int sat_add(int a, int b)
{
    int r;
    __asm__("qadd %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
