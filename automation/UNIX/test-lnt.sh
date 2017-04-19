# Test clang on UNIX using Visual Studio Team Services

set -ue
set -o pipefail
set -x

if [ -z "$LNT" ]; then
  exit 0;
fi

CLANG=${LLVM_OBJ_DIR}/bin/clang
RESULT_DATA="${LNT_RESULTS_DIR}/data.xml"
RESULT_SUMMARY="${LNT_RESULTS_DIR}/result.log"


if [ ! -e "$CLANG" ]; then
  echo "clang compiler not found at $CLANG"
  exit 1
fi

"$LNT_SCRIPT" runtest nt --sandbox "$LNT_RESULTS_DIR" --no-timestamp \
   --cc "$CLANG" --test-suite ${BUILD_SOURCESDIRECTORY}/llvm-test-suite \
   --cflags -fcheckedc-extension \
   -v --output=${RESULT_DATA} -j${BUILD_CPU_COUNT} | tee ${RESULT_SUMMARY}

if grep FAIL ${RESULT_SUMMARY}; then
  echo "LNT testing failed."
  exit 1
else
  if [ $? -eq 2 ]; then
    echo "Grep of LNT result log unexepectedly failed."
    exit 1
  fi
fi

set +ue
set +o pipefail
