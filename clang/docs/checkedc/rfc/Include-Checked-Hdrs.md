# Explicit and Implicit Inclusion of Checked Header Files
Author: Sulekha Kulkarni, Approver: David Tarditi, Date Approved:


# OARP

| Role          | Person |
|-------------------|---------------|
| Owner         | Sulekha Kulkarni  |
| Approver      | David Tarditi     |
| Reviewers     | Joseph Lloyd, Mandeep Grang, Katherine Kjeer |
| Reviewers     | Mike Hicks and other folks at CCI |


## Goal
The goal of this document is to record the design considerations for allowing
programs to include the checked counterparts of system header files either
explicitly or implicitly during the conversion of these programs to Checked C.

## Problem Summary
The existing approach for including checked header files, which is to explicitly
replace an included header file with its checked counterpart, is inconvenient
for automation by 3C. So we need a way to implicitly include checked header
files.

## Conditions that a Solution must Satisfy
 - The solution must not violate the "opt-in" philosophy that we advocate for
 the conversion of a program to Checked C. In accordance with this, the
 Checked C compiler must not silently perform an implicit inclusion of the
 checked header files.
 - The solution must be easy to integrate into a build system. 
 - The existing approach of explicitly including checked header files must
 always remain a viable alternative.
 - It should be possible to directly include certain system header files and
 bypass the inclusion of their checked counterparts in order to support custom
 build environments.
 - The order in which the compiler driver searches the include paths must remain
 unaltered.


## Details of the Existing Approach 
 - The existing approach supports only the explicit inclusion of checked header
 files.
 - An installed Checked C compiler places the checked counterparts of system
 header files like `stdio_checked.h` in the directory
 `$INSTALL_DIR/lib/clang/<VERSION>/include`, which clang searches before it
 looks in system include directories. Note that there is no alteration of the
 include search path to accommodate any include files related to Checked C.
 - If a checked header file like `stdio_checked.h` is included in a program,
 clang picks it up from the above location.

## Solution Space
 - Solution proposed by CCI:
     - Let `foo.h` be a system header file with a checked counterpart called
     `foo_checked.h`. We add a new file, also called `foo.h`, in the same
     directory that contains `foo_checked.h`. This new `foo.h` should contain
     `#include_next <foo.h>` plus all the Checked-C-specific declarations
     present in `foo_checked.h`. In addition, the original contents of
     `foo_checked.h` should be deleted and it should now only contain
     `#include <foo.h>`. 
     - The way this solution will work:
          - If a program includes `foo.h`, clang will first pick up the
          "checked" version of `foo.h`. As this `foo.h` has a
          `#include_next <foo.h>`, the system `foo.h` will be picked up
          (infinite recursion is avoided because of include_next), and all the
          Checked-C-specific declarations in the "checked" version of `foo.h`
          will also be picked up.
          - If the program includes `foo_checked.h`, the line `#include <foo.h>`
          in `foo_checked.h` will initiate the same process as above.
     - **Pros**: 1) It is convenient for automation. 2) The existing
     approach of explicit inclusion is supported. 3) No changes are required to
     the build system.
     - **Cons**: 1) It violates the "opt-in" philosophy. 2) It does not provide
     fine-grained control to directly include a system header file and avoid
     Checked-C-specific declarations. 

 - Another solution we considered:
     - Have a different compiler driver to provide the facility for implicit
     inclusion of header files.
     - **Pros**: 1) It satisfies the "opt-in" philosophy. 2) It is convenient
     for automation. 3) It provides fine-grained control to directly include a
     system header file. 4) The existing approach of explicit inclusion is
     supported. 5) In most cases integration into a build system will be easy:
     the `CC` variable can be redefined appropriately.
     - **Cons**: 1) It is a very heavy-weight solution. 2) In some cases
     integrating with a build system will be more involved when both the drivers
     are needed to compile the sources. This may happen in a scenario where most
     source files want implicit inclusion but a few source files want explicit
     inclusion because they want to directly include some system header files in
     order to avoid Checked-C-specific declarations.

## Proposed Solution
 - We propose the following solution:
     - Let `foo.h` be a system header file with its checked counterpart called
     `foo_checked.h`. We add a new file called `foo.h` in the same directory
     that contains `foo_checked.h`. In this new file, we add the following:

            #ifdef IMPLICIT_INCLUDE_CHECKED_HDRS
            #include <foo_checked.h>
            #else
            #include_next <foo.h>
            #endif
     - In `foo_checked.h`, we modify `#include <foo.h>` to
     `#include_next <foo.h>`.

 - The way this solution will work:
     - If a program includes `foo.h`, clang will pick up either `foo_checked.h`
     or the system `foo.h` depending on whether or not
     `-DIMPLICIT_INCLUDE_CHECKED_HDRS` is specified on the compilation
     commandline.  Therefore specifying this flag will cause the implicit
     inclusion of the checked counterpart of `foo.h`, which will make it
     convenient for automation. Not specifying the commandline flag will not
     perform implicit inclusion, satisfying our "opt-in" philosophy. Infinite
     recursion is avoided because of `#include_next`.
     - If a program includes `foo_checked.h` current behavior will prevail. The
     above flag has no effect on the explicit inclusion of checked header files.

 - **Pros**: 1) It satisfies the "opt-in" philosophy. 2) It is convenient for
 automation. 3) It provides fine-grained control to directly include a system
 header file. 4) The existing approach of explicit inclusion is supported. 5) In
 most cases integration with the build system is easy: the flag
 `-DIMPLICIT_INCLUDE_CHECKED_HDRS` is passed to the compilation of all 
 source files through the `CFLAGS` variable.

 - **Cons**: 1) In some cases integration with the build system will be more
 involved when the above flag needs to be passed to the compilation of most
 source files and avoided for a few source files that want to directly include
 system header files in order to avoid Checked-C-specific declarations.
