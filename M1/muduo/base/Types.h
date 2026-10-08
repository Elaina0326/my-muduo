#pragma once

#include<cstdint>
#include<string>

namespace muduo{
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
template<typename To,typename From>
inline To implicit_cast(From const &f){
    return f;
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using StringArg = const char *;
}