/* Reserves the Xbox image range inside our own executable image.
   The link merges .text/.rdata/.data/.bss into .axbe after this block,
   so the block lands at 0x11000 (image base 0x10000) and spans to 0x4B0000. */
    .section .axbe,"bwx"
    .globl _xbe_region
_xbe_region:
    .space 0x4B0000 - 0x11000
