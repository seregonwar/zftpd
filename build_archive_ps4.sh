#!/bin/bash
export LLVM_CONFIG=$(command -v llvm-config-15 || command -v llvm-config)
export LLVM_PREFIX=$(dirname $(dirname $LLVM_CONFIG))
export CC="${LLVM_PREFIX}/bin/clang"
export AR="${LLVM_PREFIX}/bin/llvm-ar"
export RANLIB="${LLVM_PREFIX}/bin/llvm-ranlib"
export CFLAGS="-target x86_64-scei-ps4 -I$(pwd)/external/ps4-payload-sdk/include"
export LDFLAGS="-target x86_64-scei-ps4 -L$(pwd)/external/ps4-payload-sdk/lib"
cd external/libarchive-3.8.6
mkdir -p build_ps4 && cd build_ps4
cmake -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_AR="$AR" \
      -DCMAKE_RANLIB="$RANLIB" \
      -DCMAKE_SYSTEM_NAME=Generic \
      -DCMAKE_C_FLAGS="$CFLAGS" \
      -DENABLE_ZLIB=OFF -DENABLE_BZip2=OFF -DENABLE_LIBXML2=OFF \
      -DENABLE_EXPAT=OFF -DENABLE_ZSTD=OFF -DENABLE_LZMA=OFF \
      -DENABLE_CNG=OFF -DENABLE_OPENSSL=OFF \
      -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
      -DPOSIX_C_SOURCE=200112L ..
make -j4
