// Tests for datafow analysis for bounds widening of _Nt_array_ptr's.
//
// RUN: %clang_cc1 -fdump-widened-bounds %s 2>1 | FileCheck %s

void f1() {
  _Nt_array_ptr<char> p : count(0) = "a";

  if (*p) {}

// CHECK: In function: f1
// CHECK: [B2]
// CHECK:   2: *p
// CHECK: [B1]
// CHECK: upper_bound(p) = 1
}

void f2() {
  _Nt_array_ptr<char> p : count(2) = "ab";

  if (*p)
    if (*(p + 1))
      if (*(p + 2)) {}

// CHECK: In function: f2
// CHECK: [B4]
// CHECK:   2: *p
// CHECK: [B3]
// CHECK:   1: *(p + 1)
// CHECK-NOT: upper_bound(p)
// CHECK: [B2]
// CHECK:   1: *(p + 2)
// CHECK-NOT: upper_bound(p)
// CHECK: [B1]
// CHECK-NOT: upper_bound(p)
}

void f3() {
  _Nt_array_ptr<char> p : count(0) = "a";
  int a;

  if (*p) {
    p = "a";
    if (a) {}
  }

// CHECK: In function: f3
// CHECK: [B3]
// CHECK:   3: *p
// CHECK: [B2]
// CHECK:   1: p = "a"
// CHECK: upper_bound(p) = 1
// CHECK: [B1]
// CHECK-NOT: upper_bound(p) = 1
}
