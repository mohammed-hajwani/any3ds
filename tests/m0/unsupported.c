/* Negative test: `setend` (switch data accesses to big-endian, ARMv6) isn't
   translated: guest memory is little-endian throughout. The recompiler must
   refuse it with the address and mnemonic. (This used to be `mul`, `umull`
   and then `qadd`, until those were added.) */
int big_endian_load(const int *p)
{
    int r;
    __asm__("setend be\n\tldr %0, [%1]\n\tsetend le" : "=r"(r) : "r"(p));
    return r;
}
