//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt --emit-type-and-global-header %s -o /dev/null | FileCheck %s
// RUN: %root/bin/revng clift-opt --emit-type-and-global-header=ptml %s -o /dev/null | %root/bin/revng ptml | FileCheck %s

!uint8_t = !clift.int<unsigned 1>

// A segment with storage keeps its plain declaration.
// CHECK: uint8_t owned_segment[8];
// CHECK-NOT: #define owned_segment

// An image-resident segment is only declared and its name refers to the image.
// CHECK: extern uint8_t resident_segment[16];
// CHECK-NEXT: #define resident_segment rr_image_resident(resident_segment, 0x56F3878)

module attributes {clift.module} {
  clift.global @owned_segment : !clift.array<8 x !uint8_t>
  attributes {
    handle = "/segment/0x1000:Generic64"
  }

  clift.global @resident_segment : !clift.array<16 x !uint8_t>
  attributes {
    clift.image_resident,
    handle = "/segment/0x56f3878:Generic64"
  }
}
