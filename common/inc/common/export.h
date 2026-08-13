#pragma once

// Export annotation for classes compiled into common.dll. Generated protobuf
// messages use COMMON_API; the macro is resolved by this header, which is
// force-included (/FI) into every translation unit that uses common.
#ifdef _WIN32
#ifdef COMMON_BUILD
#define COMMON_API __declspec(dllexport)
#else
#define COMMON_API __declspec(dllimport)
#endif
#else
#define COMMON_API
#endif
