# VimCpp.cmake — select and verify the C++ mode used for new Xim components.

include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(_xim_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(_xim_saved_required_link_options ${CMAKE_REQUIRED_LINK_OPTIONS})
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++26")
check_cxx_source_compiles([=[
    #if __cplusplus < 202400L
    # error "C++26 mode is required"
    #endif
    consteval int xim_cxx_mode()
    {
        return 26;
    }
    static_assert(xim_cxx_mode() == 26);
    int main() { return 0; }
]=] XIM_CXX26_SUPPORTED)
if(NOT XIM_CXX26_SUPPORTED)
    message(FATAL_ERROR
        "Clang must compile C++26 mode (with __cplusplus >= 202400L).")
endif()

# Prefer libc++ only after verifying that this Clang can compile, link, and
# run a C++26 program with it. On hosts without a usable libc++ installation,
# keep Clang's configured default standard library.
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -stdlib=libc++")
list(APPEND CMAKE_REQUIRED_LINK_OPTIONS -stdlib=libc++)
check_cxx_source_runs([=[
    #include <string>
    #if __cplusplus < 202400L
    # error "C++26 mode is required"
    #endif
    consteval int xim_cxx_mode()
    {
        return 26;
    }
    static_assert(xim_cxx_mode() == 26);
    int main()
    {
        const std::string value = "xim";
        return value.size() == 3 ? 0 : 1;
    }
]=] XIM_LIBCXX_CXX26_RUNS)

set(CMAKE_REQUIRED_FLAGS "${_xim_saved_required_flags}")
set(CMAKE_REQUIRED_LINK_OPTIONS ${_xim_saved_required_link_options})

if(XIM_LIBCXX_CXX26_RUNS)
    set(XIM_USE_LIBCXX TRUE)
    message(STATUS "C++26 standard library: libc++ (compile/link/run verified)")
else()
    set(XIM_USE_LIBCXX FALSE)
    message(STATUS "C++26 standard library: Clang default (libc++ not verified)")
endif()

function(xim_apply_cxx_settings target)
    if(XIM_USE_LIBCXX)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
        target_link_options(${target} PRIVATE -stdlib=libc++)
    endif()
endfunction()
