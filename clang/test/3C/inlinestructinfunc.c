// RUN: 3c -alltypes %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_ALL","CHECK" %s
// RUN: 3c %s -- | FileCheck -match-full-lines -check-prefixes="CHECK_NOALL","CHECK" %s
// RUN: 3c %s -- | %clang -c -fcheckedc-extension -x c -o /dev/null -

void foo(int *x) {
  struct bar { int *x; } *y = 0;
} 
//CHECK: struct bar { _Ptr<int> x; } *y = 0;

void baz(int *x) {
  struct bar { char *x; } *w = 0;
} 
//CHECK: struct bar { _Ptr<char> x; } *w = 0;

