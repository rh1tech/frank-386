cmake -S . -B build-host -G "MinGW Makefiles" ^
  -DELF2EZ_HOST=ON ^
  -DCMAKE_C_COMPILER=gcc.exe ^
  -DCMAKE_MAKE_PROGRAM=mingw32-make.exe ^
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-host
