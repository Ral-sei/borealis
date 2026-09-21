message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

function(check_libromfs_generator)
    if (NOT DEFINED LIBROMFS_PREBUILT_GENERATOR OR NOT EXISTS "${LIBROMFS_PREBUILT_GENERATOR}")
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libromfs-generator")
            set(LIBROMFS_PREBUILT_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/libromfs-generator" PARENT_SCOPE)
        else ()
            message(FATAL_ERROR "libromfs-generator has not been built, please refer to borealis/build_libromfs_generator.sh for more information")
        endif ()
    endif()
endfunction()

if (PLATFORM_DESKTOP)
    message(STATUS "building for Desktop")
    set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g2 -ggdb -Wall")
    set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")
    if (NOT USE_GLFW AND NOT SDL2)
        set(USE_GLFW ON)
    endif ()
elseif (PLATFORM_IOS OR PLATFORM_TVOS)
    if(PLATFORM_IOS)
        message(STATUS "building for iOS")
    elseif (PLATFORM_TVOS)
        message(STATUS "building for tvOS")
    endif ()
    set(ENABLE_STRICT_TRY_COMPILE ON)
    if (NOT DEFINED BOREALIS_LIBRARY)
        message(FATAL_ERROR BOREALIS_LIBRARY is not defined)
    endif ()
    set(CMAKE_TOOLCHAIN_FILE ${BOREALIS_LIBRARY}/cmake/ios.toolchain.cmake CACHE STRING "")
    set(USE_SDL2 ON)
    set(USE_GLFW OFF)
    if (NOT USE_GLES2)
        set(USE_GLES3 ON)
    endif()
    set(USE_LIBROMFS ON)
    if(PLATFORM_IOS)
        set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2") # iphone, ipad
    elseif(PLATFORM_TVOS)
        set(CMAKE_XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "3") # tvos
    endif()
    check_libromfs_generator()
elseif(PLATFORM_ANDROID)
    message(STATUS "building for Android")
    set(USE_SDL2 ON)
    set(USE_GLFW OFF)
    if (NOT USE_GLES3)
        set(USE_GLES2 ON)
    endif()
    set(USE_LIBROMFS ON)
    check_libromfs_generator()
elseif(PLATFORM_PSV)
    message(STATUS "building for PSVita")
    set(USE_SDL2 OFF)
    set(USE_GLFW OFF)
    if (NOT USE_GXM)
        set(USE_GLES2 ON)
        set(USE_SDL2 ON)
    endif ()
    if (NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
    endif ()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    set(VITASDK $ENV{VITASDK} CACHE BOOL "VITASDK")
    include("${VITASDK}/share/vita.cmake" REQUIRED)
    add_definitions(-D__psp2__ -D__PSV__)
    add_compile_options(-Wno-error=pedantic -Wno-psabi)
elseif(PLATFORM_PS4)
    message(STATUS "building for PS4")
    set(USE_SDL2 ON)
    set(USE_GLFW OFF)
    set(USE_GLES2 ON)
    if (NOT DEFINED ENV{OPENORBIS})
        message(FATAL_ERROR "Please set OPENORBIS in your environment. export OPENORBIS=<path to>/openorbis")
    endif ()
    set(OPENORBIS $ENV{OPENORBIS} CACHE BOOL "OPENORBIS")
    set(CMAKE_TOOLCHAIN_FILE "${OPENORBIS}/cmake/ps4.cmake" CACHE PATH "toolchain file")
    add_definitions(-DPS4 -D__PS4__)
elseif (PLATFORM_SWITCH)
    message(STATUS "building for SWITCH")
    if (NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
    endif ()
    set(DEVKITPRO $ENV{DEVKITPRO} CACHE BOOL "DEVKITPRO")
    set(__SWITCH__ ON)
    set(CMAKE_C_FLAGS "-I${DEVKITPRO}/libnx/include -I${DEVKITPRO}/portlibs/switch/include")
    set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}")
    include(${DEVKITPRO}/cmake/Switch.cmake REQUIRED)
    if (NOT USE_GLFW AND NOT SDL2)
        set(USE_GLFW ON)
    endif ()
elseif (PLATFORM_XBOX360)
    message(STATUS "building for Xbox 360/XDK")
    set(USE_SDL2 OFF)
    set(USE_GLFW OFF)
    add_definitions(-DPLATFORM_XBOX360 -D__XBOX360__ -D_XBOX360)

    # Select the freestanding Xenon compiler before project() so CMake does
    # not silently fall back to the host GCC.
    if (NOT DEFINED ENV{OXDK_LLVM})
        message(FATAL_ERROR "PLATFORM_XBOX360 requires OXDK_LLVM")
    endif ()
    set(XBOX360_LLVM "$ENV{OXDK_LLVM}")
    set(CMAKE_C_COMPILER "${XBOX360_LLVM}/bin/clang" CACHE FILEPATH "Xbox 360 C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${XBOX360_LLVM}/bin/clang++" CACHE FILEPATH "Xbox 360 C++ compiler" FORCE)
    set(CMAKE_C_COMPILER_TARGET "powerpc-unknown-none-elf" CACHE STRING "Xbox 360 compiler target" FORCE)
    set(CMAKE_CXX_COMPILER_TARGET "powerpc-unknown-none-elf" CACHE STRING "Xbox 360 C++ compiler target" FORCE)
    # Xenon has no hosted CRT startup objects. CMake's default compiler probe
    # links an executable and otherwise falls back to the host linker.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING "" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld" CACHE STRING "Xbox 360 executable linker flags" FORCE)
    # Xenon has no hosted CRT. Drive executable links through OXDK's two-pass
    # ld.lld + cxex linker instead of allowing clang/CMake to add host startup
    # objects and libc. Objects are before the literal separator; archives are
    # passed after it so oxdklink can distinguish them from its own options.
    if (NOT DEFINED ENV{OXDK_DIR})
        message(FATAL_ERROR "PLATFORM_XBOX360 requires OXDK_DIR")
    endif ()
    set(XBOX360_OXDKLINK "$ENV{OXDK_DIR}/xbox360/tools/oxdklink/oxdklink.py")
    if (NOT EXISTS "${XBOX360_OXDKLINK}")
        message(FATAL_ERROR "OXDK linker not found: ${XBOX360_OXDKLINK}")
    endif ()
    set(XBOX360_STRIP_DEPLIBS "${CMAKE_SOURCE_DIR}/cmake/strip_deplibs.cmake")
    # The link rule goes through scripts/oxdklink_xbox360.py, which copies each input
    # into a staging directory, clears .deplibs there and links the copies. Stripping
    # in place would leave every object newer than its sources, and ninja would then
    # recompile the whole project on every build.
    set(XBOX360_OXDKLINK_WRAPPER "${CMAKE_SOURCE_DIR}/scripts/oxdklink_xbox360.py")
    if (NOT EXISTS "${XBOX360_OXDKLINK_WRAPPER}")
        message(FATAL_ERROR "Xbox link wrapper not found: ${XBOX360_OXDKLINK_WRAPPER}")
    endif ()
    if (NOT EXISTS "${XBOX360_STRIP_DEPLIBS}")
        message(FATAL_ERROR "Xbox .deplibs cleanup script not found: ${XBOX360_STRIP_DEPLIBS}")
    endif ()
    set(XBOX360_XDK_LIBS "xapilib,libcMT" CACHE STRING "XDK libraries passed to oxdklink")
    set(CMAKE_CXX_LINK_EXECUTABLE
        "python3 ${XBOX360_OXDKLINK_WRAPPER} --objcopy ${XBOX360_LLVM}/bin/llvm-objcopy --oxdklink ${XBOX360_OXDKLINK} --stage ${CMAKE_BINARY_DIR}/.oxdklink/inputs -o <TARGET> -T $ENV{OXDK_DIR}/xbox360/oxdk360/title.ld --xdk $ENV{XDK_DIR} --libs ${XBOX360_XDK_LIBS} <OBJECTS> -- <LINK_LIBRARIES>"
        CACHE STRING "Xbox 360 CXX link rule" FORCE)
    set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}" CACHE STRING "Xbox 360 C link rule" FORCE)
    set(CMAKE_EXECUTABLE_SUFFIX ".xex")
    add_compile_options(
        -m32
        -ffreestanding -fno-builtin
        -fms-extensions -fms-compatibility -fshort-wchar -fno-autolink
        -D_WIN32 -D_M_PPCBE -D_M_PPC -D_PPC_ -D_XBOX -D_XENON -DXBOX
        # The XDK CRT redeclares size_t and only exposes the Microsoft
        # snprintf spellings. Keep these definitions consistent with the
        # Xbox mbedTLS target and the rest of the Xenon build.
        -D_SIZE_T_DEFINED
        -Dsnprintf=_snprintf
        -Dvsnprintf=_vsnprintf
    )
    # Match OXDK's OXDK360_LIBCXX=1 mode. The XDK contains an old MSVC/TR1
    # library which is not link-compatible with clang's Itanium ABI. C++
    # sources must see the matching libc++ headers before the XDK include tree.
    option(XBOX360_USE_OXDK_LIBCXX "Use OXDK libc++ headers/runtime" ON)
    if (XBOX360_USE_OXDK_LIBCXX)
        if (NOT DEFINED ENV{OXDK_DIR})
            message(FATAL_ERROR "XBOX360_USE_OXDK_LIBCXX requires OXDK_DIR")
        endif ()
        set(XBOX360_OXDK_ROOT "$ENV{OXDK_DIR}")
        if (DEFINED ENV{OXDK_LIBCXX_DIR})
            set(XBOX360_LIBCXX_DIR "$ENV{OXDK_LIBCXX_DIR}")
        else ()
            set(XBOX360_LIBCXX_DIR "${XBOX360_LLVM}/../libcxx/include")
        endif ()
        if (NOT EXISTS "${XBOX360_LIBCXX_DIR}/vector")
            message(FATAL_ERROR "OXDK libc++ headers not found: ${XBOX360_LIBCXX_DIR}")
        endif ()
        set(XBOX360_LIBCXX_CONFIG "${XBOX360_OXDK_ROOT}/xbox360/oxdk360/libcxx-config")
        set(XBOX360_LIBCXX_COMMON "${XBOX360_OXDK_ROOT}/common/libcxx")
        set(XBOX360_LIBCXX_CSHIM "${XBOX360_OXDK_ROOT}/xbox360/oxdk360/libcxx-cshim")
        set(XBOX360_BOREALIS_CSHIM "${CMAKE_CURRENT_LIST_DIR}/xbox360-cshim")
        if (NOT EXISTS "${XBOX360_LIBCXX_CONFIG}/__config_site")
            message(FATAL_ERROR "OXDK libc++ config not found: ${XBOX360_LIBCXX_CONFIG}")
        endif ()
        # CMake's CXX compiler already supplies its resource headers. Put them
        # after libc++ cshim, as in OXDK's xbox360.mk.
        execute_process(
            COMMAND "${XBOX360_LLVM}/bin/clang++" -print-resource-dir
            OUTPUT_VARIABLE XBOX360_CLANG_RESOURCE
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        # OXDK also puts its C shims and XDK CRT headers on the C compiler
        # path. Without this, freestanding C sources fall through to the
        # host's /usr/include headers (for example NanoVG's <stdlib.h>).
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isystem${XBOX360_BOREALIS_CSHIM} -isystem${XBOX360_LIBCXX_CSHIM} -isystem$ENV{OXDK_DIR}/xbox360/oxdk360/header-shim -isystem$ENV{XDK_DIR}/include/xbox -isystem$ENV{XDK_DIR}/TechPreview/Jul12Compiler/include/xbox -D_MSC_VER=1900 -D_MSC_FULL_VER=190023026 -D_W64= -fms-compatibility-version=19 -fdelayed-template-parsing -Xclang -target-feature -Xclang +xenon-abi")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -nostdinc++ -isystem${XBOX360_LIBCXX_CONFIG} -isystem${XBOX360_LIBCXX_COMMON} -isystem${XBOX360_BOREALIS_CSHIM} -isystem${XBOX360_LIBCXX_DIR} -isystem${XBOX360_LIBCXX_CSHIM} -isystem${XBOX360_CLANG_RESOURCE}/include -DNOMINMAX -D_MSC_VER=1900 -D_MSC_FULL_VER=190023026 -D_W64= -D_PTRDIFF_T_DEFINED -D_INTPTR_T_DEFINED -D_UINTPTR_T_DEFINED -include stdint.h -D_XM_NO_INTRINSICS_ -fno-exceptions -fms-compatibility-version=19 -fdelayed-template-parsing -Xclang -target-feature -Xclang +xenon-abi")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isystem$ENV{OXDK_DIR}/xbox360/oxdk360/header-shim -isystem$ENV{OXDK_DIR}/xbox360/oxdk360/include -isystem$ENV{XDK_DIR}/include/xbox -isystem$ENV{XDK_DIR}/TechPreview/Jul12Compiler/include/xbox")
        message(STATUS "Xbox 360 libc++ headers: ${XBOX360_LIBCXX_DIR}")

        # Compiler runtime translation units, equivalent to OXDK360_LIBCXX=1
        # mode in xbox360.mk: builtins.c supplies the 64-bit helpers clang
        # calls into, libcxx_runtime.cpp the pieces libc++ keeps out of its
        # headers. They are compiled without the project's own flags so a
        # force-included header or -D in APP_PLATFORM_OPTION cannot break
        # the toolchain's own code, exactly like the mk file's helper rules.
        set(XBOX360_BUILTINS_C "$ENV{OXDK_DIR}/xbox360/oxdk360/builtins/builtins.c")
        set(XBOX360_LIBCXX_RUNTIME_CPP "$ENV{OXDK_DIR}/xbox360/oxdk360/libcxx-shim/libcxx_runtime.cpp")
        foreach (_xbox360_rt_src IN ITEMS "${XBOX360_BUILTINS_C}" "${XBOX360_LIBCXX_RUNTIME_CPP}")
            if (NOT EXISTS "${_xbox360_rt_src}")
                message(FATAL_ERROR "OXDK runtime source not found: ${_xbox360_rt_src}")
            endif ()
        endforeach ()
        add_library(xbox360_runtime STATIC
            "${XBOX360_BUILTINS_C}"
            "${XBOX360_LIBCXX_RUNTIME_CPP}")
        target_compile_options(xbox360_runtime PRIVATE -O2)
        # No project flags/includes: only the Xenon/XDK/libc++ path set up
        # above is visible to these sources.
        set_target_properties(xbox360_runtime PROPERTIES
            CXX_STANDARD 20
            POSITION_INDEPENDENT_CODE OFF)
        message(STATUS "Xbox 360 runtime: ${XBOX360_BUILTINS_C}; ${XBOX360_LIBCXX_RUNTIME_CPP}")
    else ()
        if (DEFINED ENV{OXDK_DIR})
            include_directories(
                "$ENV{OXDK_DIR}/xbox360/oxdk360/header-shim"
                "$ENV{OXDK_DIR}/xbox360/oxdk360/include"
            )
        endif ()
        if (DEFINED ENV{XDK_DIR})
            include_directories(
                "$ENV{XDK_DIR}/include/xbox"
                "$ENV{XDK_DIR}/TechPreview/Jul12Compiler/include/xbox"
            )
        endif ()
    endif ()
else()
    message(FATAL_ERROR "Please set build target. Example: -DPLATFORM_DESKTOP=ON or -DPLATFORM_SWITCH=ON")
endif ()

# graphics drivers
if (PLATFORM_XBOX360)
    message(STATUS "Xbox 360 D3D9 driver")
    set(BOREALIS_USE_D3D9 ON)
elseif (USE_DEKO3D)
    message(STATUS "USE_DEKO3D")
    set(BOREALIS_USE_DEKO3D ON)
elseif (USE_D3D11)
    message(STATUS "USE_D3D11")
    set(BOREALIS_USE_D3D11 ON)
elseif (USE_GXM)
    message(STATUS "USE_GXM")
    set(BOREALIS_USE_GXM ON)
elseif (USE_METAL)
    message(STATUS "USE_METAL")
    set(BOREALIS_USE_METAL ON)
elseif (USE_GL2)
    message(STATUS "USE_GL2")
    add_definitions(-DUSE_GL2)
elseif (USE_GLES2)
    message(STATUS "USE_GLES2")
    add_definitions(-DUSE_GLES2)
elseif (USE_GLES3)
    message(STATUS "USE_GLES3")
    add_definitions(-DUSE_GLES3)
else ()
    message(STATUS "USE_GL3/4")
endif ()

# SDL or GLFW
if (USE_SDL2)
    message(STATUS "SDL2")
    set(USE_SDL2 ON)
    set(USE_GLFW OFF)
    add_definitions(-D__SDL2__)
elseif (USE_GLFW)
    message(STATUS "GLFW")
    set(USE_SDL2 OFF)
    set(USE_GLFW ON)
    add_definitions(-D__GLFW__)
else ()
    message(STATUS "SDL2 AND GLFW is both disabled, make sure this is what you want")
endif ()

if (USE_EGL)
    message(STATUS "Using EGL")
    add_definitions(-DUSE_EGL)
elseif (NOT USE_D3D11 AND NOT PLATFORM_XBOX360)
    message(STATUS "Using native GL")
endif ()

if (SIMPLE_HIGHLIGHT)
    message(STATUS "Enable SIMPLE_HIGHLIGHT")
    add_definitions(-DSIMPLE_HIGHLIGHT)
endif ()

if (USE_STD_THREAD)
    message(STATUS "Enable std thread")
    add_definitions(-DBOREALIS_USE_STD_THREAD)
endif ()

function(program_target target source)
    if (WIN32 AND NOT WIN32_TERMINAL)
        add_executable(${target} WIN32 ${source})
    elseif (PLATFORM_ANDROID)
        add_library(${target} SHARED)
        target_sources(${target} PRIVATE ${source})
    else ()
        add_executable(${target} ${source})
    endif ()
endfunction()

function(ios_bundle iosStoryBoard assets plist name version)
    set(IOS_CODE_SIGN_IDENTITY "" CACHE STRING "The code sign identity to use when building the IPA.")
    set(IOS_GUI_IDENTIFIER "" CACHE STRING "The package name")
    if(IOS_CODE_SIGN_IDENTITY STREQUAL "")
        set(IOS_CODE_SIGNING_ENABLED NO)
    else()
        set(IOS_CODE_SIGNING_ENABLED YES)
    endif()
    if (IOS_GUI_IDENTIFIER STREQUAL "")
        set(IOS_GUI_IDENTIFIER "${PACKAGE_NAME}")
        message(WARNING "Using default package name: ${PACKAGE_NAME}")
    endif ()
    set(IOS_SPLASH_STORYBOARD "${iosStoryBoard}")
    set(IOS_ASSETS ${assets})
    set_target_properties(${PROJECT_NAME} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${IOS_CODE_SIGN_IDENTITY}"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "${IOS_CODE_SIGNING_ENABLED}"
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "${IOS_CODE_SIGNING_ENABLED}"
            BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST ${plist}
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER ${IOS_GUI_IDENTIFIER}
            MACOSX_BUNDLE_BUNDLE_NAME ${name}
            MACOSX_BUNDLE_BUNDLE_VERSION "${version}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${version}"
            XCODE_ATTRIBUTE_ENABLE_BITCODE NO
            XCODE_ATTRIBUTE_SKIP_INSTALL NO
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            )
    set_property(
            SOURCE ${IOS_ASSETS}
            PROPERTY MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_property(
            SOURCE ${IOS_SPLASH_STORYBOARD}
            PROPERTY MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(${PROJECT_NAME} PRIVATE
            ${IOS_ASSETS}
            ${IOS_SPLASH_STORYBOARD}
            )
endfunction()

function(linux_install res res_install svg_icon icon_res_dir icon_res_list appdata desktop)
    message("install prefix: ${CMAKE_INSTALL_PREFIX}")
    # bin
    install(TARGETS ${PROJECT_NAME}
            DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
    if (NOT DEFINED CMAKE_INSTALL_SHARE_DIR)
        set(CMAKE_INSTALL_SHARE_DIR ${CMAKE_INSTALL_PREFIX}/share)
    endif ()
    # resources
    install(DIRECTORY ${res}
            DESTINATION ${res_install}
            FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # svg icon
    install(FILES ${svg_icon}
            DESTINATION ${CMAKE_INSTALL_SHARE_DIR}/icons/hicolor/scalable/apps
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # png icon
    foreach (icon_dir ${icon_res_list})
        install(FILES ${icon_res_dir}/${icon_dir}/${PACKAGE_NAME}.png
                DESTINATION ${CMAKE_INSTALL_SHARE_DIR}/icons/hicolor/${icon_dir}/apps
                PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    endforeach ()
    # appdata
    install(FILES ${appdata}
            DESTINATION ${CMAKE_INSTALL_SHARE_DIR}/metainfo
            PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    # desktop
    configure_file("${desktop}" "${CMAKE_BINARY_DIR}/${PACKAGE_NAME}.desktop")
    install(FILES "${CMAKE_BINARY_DIR}/${PACKAGE_NAME}.desktop" DESTINATION ${CMAKE_INSTALL_SHARE_DIR}/applications)
endfunction()

function(add_libromfs app res)
    message(STATUS "USE LIBROMFS")
    add_definitions(-DUSE_LIBROMFS)
    set(LIBROMFS_PROJECT_NAME ${app} PARENT_SCOPE)
    set(LIBROMFS_RESOURCE_LOCATION "${res}" PARENT_SCOPE)
endfunction()

function(git_info tag short)
    # Add git info
    find_package(Git)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --tags
                TIMEOUT 5
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_TAG_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                TIMEOUT 5
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_TAG_SHORT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
        message(STATUS "building from git tag: ${GIT_TAG_VERSION}")
        message(STATUS "building from git commit: ${GIT_TAG_SHORT}")
    else ()
        set(GIT_TAG_VERSION "")
        set(GIT_TAG_SHORT "")
    endif ()
    set(${tag} ${GIT_TAG_VERSION} PARENT_SCOPE)
    set(${short} ${GIT_TAG_SHORT} PARENT_SCOPE)
endfunction()

# switch platform
# glsl -> deko3d dksh
function(gen_dksh romfs_dir)
    find_program(NX_UAM_EXE NAMES uam HINTS "${DEVKITPRO}/tools/bin")
    if (NOT NX_UAM_EXE)
        message(FATAL_ERROR "Could not find uam: try installing uam")
    endif()
    if (NOT DEFINED BOREALIS_LIBRARY)
        message(FATAL_ERROR BOREALIS_LIBRARY is not defined)
    endif ()
    set(SHADER_BASE "${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders")

    file(MAKE_DIRECTORY "${romfs_dir}")
    execute_process(COMMAND ${NX_UAM_EXE} -s vert -o "${romfs_dir}/fill_vsh.dksh" "${SHADER_BASE}/fill_vsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_aa_fsh.dksh" "${SHADER_BASE}/fill_aa_fsh.glsl" TIMEOUT 5)
    execute_process(COMMAND ${NX_UAM_EXE} -s frag -o "${romfs_dir}/fill_fsh.dksh" "${SHADER_BASE}/fill_fsh.glsl" TIMEOUT 5)
endfunction()
