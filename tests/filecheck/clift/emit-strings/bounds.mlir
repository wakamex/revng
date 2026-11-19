//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %S/emit-strings-c %root/bin/revng %S/bounds-model.yml %s /function/0x401000:Code_x86_64 | FileCheck %s

!void = !clift.void
!uint8_t = !clift.int<unsigned 1>
!uint32_t = !clift.int<unsigned 4>
!uint64_t = !clift.int<unsigned 8>
!int64_t = !clift.int<signed 8>
!uint8_t$const = !clift.const<!uint8_t>

!seg_0x402000 = !clift.struct<
  "/type-definition/2000-StructDefinition" : size(28) {
    "/struct-field/2000-StructDefinition/0" : offset(0) !uint32_t,
    "/struct-field/2000-StructDefinition/4" : offset(4) !clift.array<12 x !uint8_t$const>,
    "/struct-field/2000-StructDefinition/16" : offset(16) !clift.array<4 x !uint8_t$const>
  }
>

!fun_0x401000 = !clift.func<
  "/type-definition/1000-CABIFunctionDefinition" : !void(!uint64_t)
>

#data_model = #clift.data_model<
  pointer = 8,
  char = 1,
  short = 2,
  int = 4,
  long = 8,
  long long = 8,
  float = 4,
  double = 8,
  long double = 16
>

module attributes {clift.module, clift.data_model = #data_model} {
  clift.global @seg_0x402000 : !clift.const<!seg_0x402000> attributes {
    handle = "/segment/0x402000:Generic64"
  }

  // CHECK: void function_0x401000_Code_x86_64(uint64_t argument_0) {
  clift.func @fun_0x401000<!fun_0x401000>(%arg0 : !uint64_t) attributes {
    handle = "/function/0x401000:Code_x86_64"
  } {
    // In-bounds decay to pointer.
    // CHECK-NEXT: (const uint8_t *) "hello world";
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      clift.yield %2 : !clift.ptr<8 to !uint8_t$const>
    }

    // In-bounds 8-byte read at offset 0 of a 12-byte string.
    // CHECK-NEXT: *(uint64_t *) &"hello world";
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !clift.ptr<8 to !uint64_t>
      %4 = clift.indirection %3 : !clift.ptr<8 to !uint64_t>
      clift.yield %4 : !uint64_t
    }

    // In-bounds 8-byte read at offset 4 of a 12-byte string ([4, 12) <= 12).
    // CHECK-NEXT: *(uint64_t *) ((uint64_t) & "hello world" + 4UL);
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !uint64_t
      %4 = clift.imm 4 : !uint64_t
      %5 = clift.add %3, %4 : !uint64_t
      %6 = clift.bitcast %5 : !uint64_t -> !clift.ptr<8 to !uint64_t>
      %7 = clift.indirection %6 : !clift.ptr<8 to !uint64_t>
      clift.yield %7 : !uint64_t
    }

    // In-bounds constant byte subscript.
    // CHECK-NEXT: &((const uint8_t *) "hello world")[5UL];
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      %3 = clift.imm 5 : !uint64_t
      %4 = clift.subscript %2, %3 : (!clift.ptr<8 to !uint8_t$const>, !uint64_t)
      %5 = clift.addressof %4 : !clift.ptr<8 to !uint8_t$const>
      clift.yield %5 : !clift.ptr<8 to !uint8_t$const>
    }

    // In-bounds cast to void pointer.
    // CHECK-NEXT: (void *) &"hello world";
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !clift.ptr<8 to !void>
      clift.yield %3 : !clift.ptr<8 to !void>
    }

    // Out-of-bounds ptr_add + 8-byte dereference ([8, 16) > 12).
    // CHECK-NEXT: *((uint64_t *) &segment_1.offset_4 + 1UL);
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !clift.ptr<8 to !uint64_t>
      %4 = clift.imm 1 : !uint64_t
      %5 = clift.ptr_add %3, %4 : (!clift.ptr<8 to !uint64_t>, !uint64_t)
      %6 = clift.indirection %5 : !clift.ptr<8 to !uint64_t>
      clift.yield %6 : !uint64_t
    }

    // Out-of-bounds escaping uint64_t pointer at offset 8 ([8, 16) > 12).
    // CHECK-NEXT: (uint64_t *) &segment_1.offset_4 + 1UL;
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !clift.ptr<8 to !uint64_t>
      %4 = clift.imm 1 : !uint64_t
      %5 = clift.ptr_add %3, %4 : (!clift.ptr<8 to !uint64_t>, !uint64_t)
      clift.yield %5 : !clift.ptr<8 to !uint64_t>
    }

    // Out-of-bounds integer addition + 8-byte dereference ([8, 16) > 12).
    // CHECK-NEXT: *(uint64_t *) ((uint64_t) &segment_1.offset_4 + 8UL);
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<12 x !uint8_t$const>>> -> !uint64_t
      %4 = clift.imm 8 : !uint64_t
      %5 = clift.add %3, %4 : !uint64_t
      %6 = clift.bitcast %5 : !uint64_t -> !clift.ptr<8 to !uint64_t>
      %7 = clift.indirection %6 : !clift.ptr<8 to !uint64_t>
      clift.yield %7 : !uint64_t
    }

    // Negative offset via ptr_sub.
    // CHECK-NEXT: *((const uint8_t *) segment_1.offset_4 - 1UL);
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      %3 = clift.imm 1 : !uint64_t
      %4 = clift.ptr_sub %2, %3 : (!clift.ptr<8 to !uint8_t$const>, !uint64_t)
      %5 = clift.indirection %4 : !clift.ptr<8 to !uint8_t$const>
      clift.yield %5 : !uint8_t$const
    }

    // Negative offset via signed ptr_add.
    // CHECK-NEXT: *((const uint8_t *) segment_1.offset_4 + 18446744073709551615L);
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      %3 = clift.imm -1 : !int64_t
      %4 = clift.ptr_add %2, %3 : (!clift.ptr<8 to !uint8_t$const>, !int64_t)
      %5 = clift.indirection %4 : !clift.ptr<8 to !uint8_t$const>
      clift.yield %5 : !uint8_t$const
    }

    // Dynamic subscript index (cannot prove in-bounds).
    // CHECK-NEXT: *(uint32_t *) &((const uint8_t *) segment_1.offset_4)[argument_0];
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      %3 = clift.subscript %2, %arg0 : (!clift.ptr<8 to !uint8_t$const>, !uint64_t)
      %4 = clift.addressof %3 : !clift.ptr<8 to !uint8_t$const>
      %5 = clift.bitcast %4 : !clift.ptr<8 to !uint8_t$const> -> !clift.ptr<8 to !uint32_t>
      %6 = clift.indirection %5 : !clift.ptr<8 to !uint32_t>
      clift.yield %6 : !uint32_t
    }

    // Over-wide 8-byte read at offset 0 of a 4-byte string ([0, 8) > 4).
    // CHECK-NEXT: *(uint64_t *) &segment_1.offset_16;
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<2> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<4 x !uint8_t$const>>
      %2 = clift.addressof %1 : !clift.ptr<8 to !clift.const<!clift.array<4 x !uint8_t$const>>>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !clift.const<!clift.array<4 x !uint8_t$const>>> -> !clift.ptr<8 to !uint64_t>
      %4 = clift.indirection %3 : !clift.ptr<8 to !uint64_t>
      clift.yield %4 : !uint64_t
    }

    // Escaping pointer past the end of the string (offset 12 >= 12).
    // CHECK-NEXT: (const uint8_t *) segment_1.offset_4 + 12UL;
    clift.expr {
      %0 = clift.use @seg_0x402000 : !clift.const<!seg_0x402000>
      %1 = clift.access<1> %0 : !clift.const<!seg_0x402000> -> !clift.const<!clift.array<12 x !uint8_t$const>>
      %2 = clift.decay %1 : !clift.const<!clift.array<12 x !uint8_t$const>> -> !clift.ptr<8 to !uint8_t$const>
      %3 = clift.imm 12 : !uint64_t
      %4 = clift.ptr_add %2, %3 : (!clift.ptr<8 to !uint8_t$const>, !uint64_t)
      clift.yield %4 : !clift.ptr<8 to !uint8_t$const>
    }
  }
  // CHECK-NEXT: }
}
