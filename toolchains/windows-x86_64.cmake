# cmake .. -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=../toolchains/x86_64-windows.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "gcc")
set(CMAKE_CXX_COMPILER "g++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

add_definitions(-DWEBRTC_WIN)
add_compile_options(-mavx2 -mfma)

# 静态链接 GCC 运行时（libgcc / libstdc++ / winpthread）。
# 不这样做的话，产物依赖 libwinpthread-1.dll 等 MinGW 运行库，而 Git Bash 的
# /mingw64/bin 里有一份同名但版本不同的 DLL 会抢先被加载，导致
# STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139) —— 测试必须能从裸 shell 跑起来。
# 用 CACHE FORCE 而不是 _INIT：工具链文件每次 configure 都会重读，_INIT 只在
# 首次写入缓存时生效，改了这里却不清 build/ 不会重新链接。
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++"
    CACHE STRING "Aura: MinGW 运行时静态链接（见上）" FORCE)