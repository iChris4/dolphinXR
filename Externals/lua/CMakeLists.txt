# Lua 5.4.8 (https://www.lua.org/ftp/lua-5.4.8.tar.gz, sha256
# 4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae), used by the VR game patches
# (Source/Core/Core/VRPatch). Only the `src` directory is vendored, without the interpreter and
# compiler programs (lua.c, luac.c) and without the libraries a sandboxed patch must never reach:
# io (liolib.c), os (loslib.c), package (loadlib.c), debug (ldblib.c) and linit.c, which opens them.
add_library(lua STATIC
  src/lapi.c
  src/lauxlib.c
  src/lbaselib.c
  src/lcode.c
  src/lcorolib.c
  src/lctype.c
  src/ldebug.c
  src/ldo.c
  src/ldump.c
  src/lfunc.c
  src/lgc.c
  src/llex.c
  src/lmathlib.c
  src/lmem.c
  src/lobject.c
  src/lopcodes.c
  src/lparser.c
  src/lstate.c
  src/lstring.c
  src/lstrlib.c
  src/ltable.c
  src/ltablib.c
  src/ltm.c
  src/lundump.c
  src/lutf8lib.c
  src/lvm.c
  src/lzio.c
)
dolphin_disable_warnings(lua)

target_include_directories(lua
PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The core library links into Android's shared libmain.so.
set_target_properties(lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(UNIX)
  # POSIX features only (mkstemp, popen...); LUA_USE_LINUX would also enable dlopen.
  target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
endif()
