#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(__x86_64__)
#include <x86intrin.h>
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
