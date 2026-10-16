# Exception: boost-ext/ut registers argv through constructor(101), which Mach-O lacks.
file(READ include/boost/ut.hpp text)
string(REPLACE "!defined(__EMSCRIPTEN__)\n" "!defined(__EMSCRIPTEN__) && !defined(__APPLE__)\n" text "${text}")
file(WRITE include/boost/ut.hpp "${text}")
