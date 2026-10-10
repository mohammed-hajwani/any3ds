/* Negative test: compiles to `umull` (32x32 -> 64-bit multiply), which the
   recompiler doesn't translate yet. It must refuse it with the address and
   mnemonic. (This used to be `mul`, until M4 added that.) */
unsigned long long wide_mul(unsigned a, unsigned b) { return (unsigned long long)a * b; }
