# ascify

6. Install hipify:

    ```bash
    cmake -G Ninja   -DCMAKE_INSTALL_PREFIX=$(pwd)/../ascify_install   -DCMAKE_BUILD_TYPE=Release   \
    -DCMAKE_PREFIX_PATH=$(pwd)/../../../llvm-project/build_docker/build   \
    -DCMAKE_PREFIX_PATH=$(pwd)/../../../llvm-project/clang
    ../../
    ```