/* Negative test: compiles to `mul`, which M0 does not translate.
   The recompiler must refuse it with the address and mnemonic. */
int mul(int a, int b) { return a * b; }
