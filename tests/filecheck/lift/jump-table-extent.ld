/* This file is distributed under the MIT License. See LICENSE.md for details.
 * Fixed addresses for code, the jump tables and the unwind information.
 */
ENTRY(entry)
PHDRS {
  headers PT_LOAD FILEHDR PHDRS FLAGS(4);
  code PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(4);
}
SECTIONS {
  . = 0x400000 + SIZEOF_HEADERS;
  .header : { BYTE(0) } :headers
  . = 0x401000;
  .text : { *(.text) } :code
  . = 0x402000;
  .rodata : { *(.rodata) } :data
  .eh_frame : { *(.eh_frame) } :data
}
