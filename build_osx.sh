#!/bin/zsh

export PATH=/usr/local/bin:${PATH}

LLVM_PATH=${HOME}/usr/llvm

export CC=${LLVM_PATH}/bin/clang
export CXX=${LLVM_PATH}/bin/clang++

CURRENT_DIR=`pwd`
BUILD_PATH=cmake-build-release

# 如何没有remote cpp-package就创建
# 检查是否已存在cpp-package remote
# conan remote list | grep -q cpp-package || conan remote add cpp-package http://10.18.80.15:8081/repository/cpp-package/


# ## 如何没有默认的profile文件则创建
# if [ ! -f ${HOME}/.conan2/profiles/default ]; then
#     conan profile detect
# fi

## 拉取包
# conan install ./conanfile_osx.txt --output-folder=${BUILD_PATH} --build=missing \
#     --profile=${CURRENT_DIR}/profiles/macOS/clang_profile -r cpp-package \
#     -c tools.cmake.cmaketoolchain:extra_variables="{'CMAKE_OSX_SYSROOT':'${OSX_SYSROOT}'}"

LIBBASE_PATH=${HOME}/usr/libbase-osx-dev
UNIDES_THIRD_PARTY_PATH=${HOME}/usr/unides_thrid_party_dev
OSX_SYSROOT=`xcrun --show-sdk-path`
RANLIB_PATH=`xcrun --find ranlib`

if [ $# = 0 ] || [ $1 != 'Debug' ] ; then 

cmake -B ${BUILD_PATH} -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DLLVM_PATH=${LLVM_PATH} \
    -DLIBBASE_PATH=${LIBBASE_PATH} \
    -DUNIDES_THIRD_PARTY_PATH=${UNIDES_THIRD_PARTY_PATH} \
    -DCMAKE_OSX_ARCHITECTURES="arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.5 \
    -DCMAKE_RANLIB=${RANLIB_PATH} \
    -DENABLE_CODE_COVERAGE=OFF

else 

cmake -B ${BUILD_PATH} -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=${CC} \
    -DCMAKE_CXX_COMPILER=${CXX} \
    -DLLVM_PATH=${LLVM_PATH} \
    -DLIBBASE_PATH=${LIBBASE_PATH} \
    -DUNIDES_THIRD_PARTY_PATH=${UNIDES_THIRD_PARTY_PATH} \
    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.5 \
    -DCMAKE_RANLIB=${RANLIB_PATH} \
    -DENABLE_CODE_COVERAGE=ON
fi

#编译
ninja -C ${BUILD_PATH} 