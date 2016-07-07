// Checked C extension is supported for C.   Make sure driver
// accepts the flag for C and rejects it when the file is
// compiled as another language.
//
// RUN: %clang -c -fcheckedc-extension %s
// RUN: %clang_cl -c -Xclang -fcheckedc-extension %s
//
// Have clang compile this file as C++ file.
// RUN: not %clang -c -fcheckedc-extension -x c++ %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=check-cpp
// check-cpp: error: invalid argument '-fcheckedc-extension' not allowed with 'C++'
//
// Have clang-cl compile this file as a C++ file.
// RUN: not %clang_cl -c -Xclang -fcheckedc-extension /TP %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=clcheck-cpp
// clcheck-cpp: error: invalid argument '-fcheckedc-extension' not allowed with 'C++'
//
// RUN: not %clang -c -fcheckedc-extension -x cuda %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=check-cuda
// check-cuda: error: invalid argument '-fcheckedc-extension' not allowed with 'CUDA'
//
// RUN: not %clang -c -fcheckedc-extension -x cl %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=check-opencl
// check-opencl: error: invalid argument '-fcheckedc-extension' not allowed with 'OpenCL'
//
// RUN: not %clang -c -fcheckedc-extension -x objective-c %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=check-objc
// check-objc: error: invalid argument '-fcheckedc-extension' not allowed with 'Objective C'
//
// RUN: not %clang -c -fcheckedc-extension -x objective-c++ %s 2>&1 \
// RUN:  | FileCheck %s -check-prefix=check-objcpp
// check-objcpp: error: invalid argument '-fcheckedc-extension' not allowed with 'Objective C/C++'


extern void f(ptr<int> p) {}
