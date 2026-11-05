# FFmpeg (LGPL 2.1 or later), built from its release source as an external project: only what the
# engines' movies need (the ASF demuxer and the Windows Media video and audio decoders), as static
# libraries, without assembly on x86 (no nasm needed). Gives the INTERFACE target `nyan_ffmpeg`.
#
# The tarball is downloaded once into third_party/downloads (git-ignored); set NYAN_FFMPEG_URL to a
# local copy (file:///...) to build offline. Release 7.1.1, checked against FFmpeg's release signing
# key (FCF986EA15E6E293A5644F10B4322F04D67658D8).

include(ExternalProject)
enable_language(C)

set(NYAN_FFMPEG_URL "https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz" CACHE STRING "FFmpeg source tarball")
set(NYAN_FFMPEG_SHA256 "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1")

set(_ff_prefix "${CMAKE_BINARY_DIR}/ffmpeg")
set(_ff_flags
  --prefix=${_ff_prefix}
  --enable-static --disable-shared --enable-pic
  --disable-programs --disable-doc --disable-network --disable-autodetect --disable-iconv
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-postproc
  --disable-everything
  --enable-demuxer=asf
  --enable-decoder=wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro
  --cc=${CMAKE_C_COMPILER})

if(ANDROID)
  # The NDK's clang for the app's ABI and API level.
  list(APPEND _ff_flags
    --enable-cross-compile --target-os=android --arch=aarch64 --cpu=armv8-a
    --sysroot=${CMAKE_SYSROOT}
    # Hidden symbols: linked into the app's shared library, the NEON code's PC-relative references to
    # FFmpeg's tables must not be preemptible.
    "--extra-cflags=--target=${CMAKE_C_COMPILER_TARGET} -fvisibility=hidden"
    "--extra-ldflags=--target=${CMAKE_C_COMPILER_TARGET}"
    --ar=${CMAKE_AR} --nm=${CMAKE_NM} --ranlib=${CMAKE_RANLIB} --strip=${CMAKE_STRIP})
else()
  list(APPEND _ff_flags --disable-x86asm)
endif()

set(_ff_libs
  ${_ff_prefix}/lib/libavformat.a
  ${_ff_prefix}/lib/libavcodec.a
  ${_ff_prefix}/lib/libavutil.a)

ExternalProject_Add(ffmpeg_external
  URL ${NYAN_FFMPEG_URL}
  URL_HASH SHA256=${NYAN_FFMPEG_SHA256}
  DOWNLOAD_DIR ${PROJECT_SOURCE_DIR}/third_party/downloads
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  PREFIX ${CMAKE_BINARY_DIR}/ffmpeg-build
  CONFIGURE_COMMAND <SOURCE_DIR>/configure ${_ff_flags}
  BUILD_COMMAND make -j8
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${_ff_libs})

file(MAKE_DIRECTORY ${_ff_prefix}/include)
add_library(nyan_ffmpeg INTERFACE)
add_dependencies(nyan_ffmpeg ffmpeg_external)
target_include_directories(nyan_ffmpeg SYSTEM INTERFACE ${_ff_prefix}/include)
find_package(Threads REQUIRED)
target_link_libraries(nyan_ffmpeg INTERFACE ${_ff_libs} Threads::Threads m)
