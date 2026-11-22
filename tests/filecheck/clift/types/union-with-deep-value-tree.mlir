//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Each union holds two of the next by value. The check for recursive class
// types visits each union once, not once for each of the 2^40 paths to the last.

// RUN: %root/bin/revng clift-opt %s | FileCheck %s

!U40 = !clift.union<"/type-definition/40-UnionDefinition" : {
  "/union-field/40-UnionDefinition/0" : !clift.int<signed 4>
}>

!U39 = !clift.union<"/type-definition/39-UnionDefinition" : {
  "/union-field/39-UnionDefinition/0" : !U40,
  "/union-field/39-UnionDefinition/1" : !U40
}>

!U38 = !clift.union<"/type-definition/38-UnionDefinition" : {
  "/union-field/38-UnionDefinition/0" : !U39,
  "/union-field/38-UnionDefinition/1" : !U39
}>

!U37 = !clift.union<"/type-definition/37-UnionDefinition" : {
  "/union-field/37-UnionDefinition/0" : !U38,
  "/union-field/37-UnionDefinition/1" : !U38
}>

!U36 = !clift.union<"/type-definition/36-UnionDefinition" : {
  "/union-field/36-UnionDefinition/0" : !U37,
  "/union-field/36-UnionDefinition/1" : !U37
}>

!U35 = !clift.union<"/type-definition/35-UnionDefinition" : {
  "/union-field/35-UnionDefinition/0" : !U36,
  "/union-field/35-UnionDefinition/1" : !U36
}>

!U34 = !clift.union<"/type-definition/34-UnionDefinition" : {
  "/union-field/34-UnionDefinition/0" : !U35,
  "/union-field/34-UnionDefinition/1" : !U35
}>

!U33 = !clift.union<"/type-definition/33-UnionDefinition" : {
  "/union-field/33-UnionDefinition/0" : !U34,
  "/union-field/33-UnionDefinition/1" : !U34
}>

!U32 = !clift.union<"/type-definition/32-UnionDefinition" : {
  "/union-field/32-UnionDefinition/0" : !U33,
  "/union-field/32-UnionDefinition/1" : !U33
}>

!U31 = !clift.union<"/type-definition/31-UnionDefinition" : {
  "/union-field/31-UnionDefinition/0" : !U32,
  "/union-field/31-UnionDefinition/1" : !U32
}>

!U30 = !clift.union<"/type-definition/30-UnionDefinition" : {
  "/union-field/30-UnionDefinition/0" : !U31,
  "/union-field/30-UnionDefinition/1" : !U31
}>

!U29 = !clift.union<"/type-definition/29-UnionDefinition" : {
  "/union-field/29-UnionDefinition/0" : !U30,
  "/union-field/29-UnionDefinition/1" : !U30
}>

!U28 = !clift.union<"/type-definition/28-UnionDefinition" : {
  "/union-field/28-UnionDefinition/0" : !U29,
  "/union-field/28-UnionDefinition/1" : !U29
}>

!U27 = !clift.union<"/type-definition/27-UnionDefinition" : {
  "/union-field/27-UnionDefinition/0" : !U28,
  "/union-field/27-UnionDefinition/1" : !U28
}>

!U26 = !clift.union<"/type-definition/26-UnionDefinition" : {
  "/union-field/26-UnionDefinition/0" : !U27,
  "/union-field/26-UnionDefinition/1" : !U27
}>

!U25 = !clift.union<"/type-definition/25-UnionDefinition" : {
  "/union-field/25-UnionDefinition/0" : !U26,
  "/union-field/25-UnionDefinition/1" : !U26
}>

!U24 = !clift.union<"/type-definition/24-UnionDefinition" : {
  "/union-field/24-UnionDefinition/0" : !U25,
  "/union-field/24-UnionDefinition/1" : !U25
}>

!U23 = !clift.union<"/type-definition/23-UnionDefinition" : {
  "/union-field/23-UnionDefinition/0" : !U24,
  "/union-field/23-UnionDefinition/1" : !U24
}>

!U22 = !clift.union<"/type-definition/22-UnionDefinition" : {
  "/union-field/22-UnionDefinition/0" : !U23,
  "/union-field/22-UnionDefinition/1" : !U23
}>

!U21 = !clift.union<"/type-definition/21-UnionDefinition" : {
  "/union-field/21-UnionDefinition/0" : !U22,
  "/union-field/21-UnionDefinition/1" : !U22
}>

!U20 = !clift.union<"/type-definition/20-UnionDefinition" : {
  "/union-field/20-UnionDefinition/0" : !U21,
  "/union-field/20-UnionDefinition/1" : !U21
}>

!U19 = !clift.union<"/type-definition/19-UnionDefinition" : {
  "/union-field/19-UnionDefinition/0" : !U20,
  "/union-field/19-UnionDefinition/1" : !U20
}>

!U18 = !clift.union<"/type-definition/18-UnionDefinition" : {
  "/union-field/18-UnionDefinition/0" : !U19,
  "/union-field/18-UnionDefinition/1" : !U19
}>

!U17 = !clift.union<"/type-definition/17-UnionDefinition" : {
  "/union-field/17-UnionDefinition/0" : !U18,
  "/union-field/17-UnionDefinition/1" : !U18
}>

!U16 = !clift.union<"/type-definition/16-UnionDefinition" : {
  "/union-field/16-UnionDefinition/0" : !U17,
  "/union-field/16-UnionDefinition/1" : !U17
}>

!U15 = !clift.union<"/type-definition/15-UnionDefinition" : {
  "/union-field/15-UnionDefinition/0" : !U16,
  "/union-field/15-UnionDefinition/1" : !U16
}>

!U14 = !clift.union<"/type-definition/14-UnionDefinition" : {
  "/union-field/14-UnionDefinition/0" : !U15,
  "/union-field/14-UnionDefinition/1" : !U15
}>

!U13 = !clift.union<"/type-definition/13-UnionDefinition" : {
  "/union-field/13-UnionDefinition/0" : !U14,
  "/union-field/13-UnionDefinition/1" : !U14
}>

!U12 = !clift.union<"/type-definition/12-UnionDefinition" : {
  "/union-field/12-UnionDefinition/0" : !U13,
  "/union-field/12-UnionDefinition/1" : !U13
}>

!U11 = !clift.union<"/type-definition/11-UnionDefinition" : {
  "/union-field/11-UnionDefinition/0" : !U12,
  "/union-field/11-UnionDefinition/1" : !U12
}>

!U10 = !clift.union<"/type-definition/10-UnionDefinition" : {
  "/union-field/10-UnionDefinition/0" : !U11,
  "/union-field/10-UnionDefinition/1" : !U11
}>

!U9 = !clift.union<"/type-definition/9-UnionDefinition" : {
  "/union-field/9-UnionDefinition/0" : !U10,
  "/union-field/9-UnionDefinition/1" : !U10
}>

!U8 = !clift.union<"/type-definition/8-UnionDefinition" : {
  "/union-field/8-UnionDefinition/0" : !U9,
  "/union-field/8-UnionDefinition/1" : !U9
}>

!U7 = !clift.union<"/type-definition/7-UnionDefinition" : {
  "/union-field/7-UnionDefinition/0" : !U8,
  "/union-field/7-UnionDefinition/1" : !U8
}>

!U6 = !clift.union<"/type-definition/6-UnionDefinition" : {
  "/union-field/6-UnionDefinition/0" : !U7,
  "/union-field/6-UnionDefinition/1" : !U7
}>

!U5 = !clift.union<"/type-definition/5-UnionDefinition" : {
  "/union-field/5-UnionDefinition/0" : !U6,
  "/union-field/5-UnionDefinition/1" : !U6
}>

!U4 = !clift.union<"/type-definition/4-UnionDefinition" : {
  "/union-field/4-UnionDefinition/0" : !U5,
  "/union-field/4-UnionDefinition/1" : !U5
}>

!U3 = !clift.union<"/type-definition/3-UnionDefinition" : {
  "/union-field/3-UnionDefinition/0" : !U4,
  "/union-field/3-UnionDefinition/1" : !U4
}>

!U2 = !clift.union<"/type-definition/2-UnionDefinition" : {
  "/union-field/2-UnionDefinition/0" : !U3,
  "/union-field/2-UnionDefinition/1" : !U3
}>

!U1 = !clift.union<"/type-definition/1-UnionDefinition" : {
  "/union-field/1-UnionDefinition/0" : !U2,
  "/union-field/1-UnionDefinition/1" : !U2
}>

// CHECK: clift.test
module attributes {clift.module, clift.test = !U1} {}
