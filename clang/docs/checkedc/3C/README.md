# 3C: Semi-automated conversion of C code to Checked C

The 3C (**C**hecked-**C**-**C**onvert) software facilitates conversion
of existing C code to Checked C by automatically inferring as many
Checked C annotations as possible and guiding the developer through
the process of assigning the remaining ones. 3C aims to provide the
first feasible way for organizations with large legacy C codebases
(that they don't want to drop everything to rewrite in a better
language) to comprehensively verify their code's spatial memory
safety.

This document describes 3C in general. Here are the [build
instructions](INSTALL.md). The inference logic is implemented in the
[`clang/lib/3C` directory](../../../lib/3C) in the form of a library
that can potentially be used in multiple contexts. As of November
2020, the only way to use 3C is via the `3c` command line tool in the
[`clang/tools/3c` directory](../../../tools/3c); its usage is
documented in [the readme there](../../../tools/3c/README.md). There
is also code for a custom build of `clangd` named `clangd3c` that can
be used to interactively convert code from within an IDE, but it is
currently unmaintained and believed not to work; we may revive it at
some point.

## What 3C users should know about the development process

Development of 3C is led by [Correct Computation,
Inc.](https://correctcomputation.com/) (CCI) in the
https://github.com/correctcomputation/checkedc-clang repository, which
is a fork of Microsoft's Checked C repository
(https://github.com/microsoft/checkedc-clang). Issues and pull
requests related to 3C should be submitted to the CCI repository; see
[CONTRIBUTING.md](CONTRIBUTING.md) for more information. Changes are
merged frequently from Microsoft's repository to CCI's and less
frequently in the opposite direction. While some automated tests of 3C
are run in Microsoft's repository, the coverage of these tests is
currently mediocre, so changes to Microsoft's repository may break
some functionality of its copy of 3C. Thus, users of 3C can choose
either CCI's repository (for the latest 3C with a somewhat older
Checked C) or Microsoft's (for the latest Checked C with 3C that is
significantly older and possibly broken). This workflow may change in
the future.

As of November 2020, 3C is pre-alpha quality and we are just starting
to establish its public presence and processes. CCI is also working on
a proprietary extension of 3C called 5C ("**C**orrect
**C**omputation's **C**hecked-**C**-**C**onvert"). Our current plan is
that 3C will contain the core inference logic, while 5C will add
features to enhance developer productivity. If you'd like more
information about 5C, please contact us at
info@correctcomputation.com.
