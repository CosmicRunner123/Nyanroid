# Libraries the Diesel engine vendors (see NYANROID.md here): Squirrel 2.2.4 (the games' script VM,
# with Nitroplus's additions), zstd 1.5.7 (the NPK3 archives; the saves' MWCF files) and libwebp 1.3.1's decoder
# (the games' pictures). Gives the static library `nyan_diesel_vendor`, linked into nyan_core.

enable_language(C)
set(_dv ${CMAKE_CURRENT_LIST_DIR})

set(_dv_squirrel
  squirrel/squirrel/sqapi.cpp
  squirrel/squirrel/sqbaselib.cpp
  squirrel/squirrel/sqclass.cpp
  squirrel/squirrel/sqcompiler.cpp
  squirrel/squirrel/sqdebug.cpp
  squirrel/squirrel/sqfuncstate.cpp
  squirrel/squirrel/sqlexer.cpp
  squirrel/squirrel/sqmem.cpp
  squirrel/squirrel/sqobject.cpp
  squirrel/squirrel/sqstate.cpp
  squirrel/squirrel/sqtable.cpp
  squirrel/squirrel/sqvm.cpp
  squirrel/sqstdlib/sqstdaux.cpp
  squirrel/sqstdlib/sqstdblob.cpp
  squirrel/sqstdlib/sqstdio.cpp
  squirrel/sqstdlib/sqstdmath.cpp
  squirrel/sqstdlib/sqstdrex.cpp
  squirrel/sqstdlib/sqstdstream.cpp
  squirrel/sqstdlib/sqstdstring.cpp
  squirrel/sqstdlib/sqstdsystem.cpp)

set(_dv_zstd
  zstd/lib/common/debug.c
  zstd/lib/common/entropy_common.c
  zstd/lib/common/error_private.c
  zstd/lib/common/fse_decompress.c
  zstd/lib/common/xxhash.c
  zstd/lib/common/zstd_common.c
  zstd/lib/decompress/huf_decompress.c
  zstd/lib/decompress/zstd_ddict.c
  zstd/lib/decompress/zstd_decompress.c
  zstd/lib/decompress/zstd_decompress_block.c
  zstd/lib/compress/fse_compress.c
  zstd/lib/compress/hist.c
  zstd/lib/compress/huf_compress.c
  zstd/lib/compress/zstd_compress.c
  zstd/lib/compress/zstd_compress_literals.c
  zstd/lib/compress/zstd_compress_sequences.c
  zstd/lib/compress/zstd_compress_superblock.c
  zstd/lib/compress/zstd_double_fast.c
  zstd/lib/compress/zstd_fast.c
  zstd/lib/compress/zstd_lazy.c
  zstd/lib/compress/zstd_ldm.c
  zstd/lib/compress/zstd_opt.c
  zstd/lib/compress/zstd_preSplit.c)

set(_dv_webp
  webp/src/dec/alpha_dec.c
  webp/src/dec/buffer_dec.c
  webp/src/dec/frame_dec.c
  webp/src/dec/idec_dec.c
  webp/src/dec/io_dec.c
  webp/src/dec/quant_dec.c
  webp/src/dec/tree_dec.c
  webp/src/dec/vp8_dec.c
  webp/src/dec/vp8l_dec.c
  webp/src/dec/webp_dec.c
  webp/src/dsp/alpha_processing.c
  webp/src/dsp/alpha_processing_neon.c
  webp/src/dsp/alpha_processing_sse2.c
  webp/src/dsp/alpha_processing_sse41.c
  webp/src/dsp/cpu.c
  webp/src/dsp/dec.c
  webp/src/dsp/dec_clip_tables.c
  webp/src/dsp/dec_neon.c
  webp/src/dsp/dec_sse2.c
  webp/src/dsp/dec_sse41.c
  webp/src/dsp/filters.c
  webp/src/dsp/filters_neon.c
  webp/src/dsp/filters_sse2.c
  webp/src/dsp/lossless.c
  webp/src/dsp/lossless_neon.c
  webp/src/dsp/lossless_sse2.c
  webp/src/dsp/lossless_sse41.c
  webp/src/dsp/rescaler.c
  webp/src/dsp/rescaler_neon.c
  webp/src/dsp/rescaler_sse2.c
  webp/src/dsp/upsampling.c
  webp/src/dsp/upsampling_neon.c
  webp/src/dsp/upsampling_sse2.c
  webp/src/dsp/upsampling_sse41.c
  webp/src/dsp/yuv.c
  webp/src/dsp/yuv_neon.c
  webp/src/dsp/yuv_sse2.c
  webp/src/dsp/yuv_sse41.c
  webp/src/utils/bit_reader_utils.c
  webp/src/utils/color_cache_utils.c
  webp/src/utils/filters_utils.c
  webp/src/utils/huffman_utils.c
  webp/src/utils/palette.c
  webp/src/utils/quant_levels_dec_utils.c
  webp/src/utils/random_utils.c
  webp/src/utils/rescaler_utils.c
  webp/src/utils/thread_utils.c
  webp/src/utils/utils.c)

list(TRANSFORM _dv_squirrel PREPEND ${_dv}/)
list(TRANSFORM _dv_zstd PREPEND ${_dv}/)
list(TRANSFORM _dv_webp PREPEND ${_dv}/)

add_library(nyan_diesel_vendor STATIC ${_dv_squirrel} ${_dv_zstd} ${_dv_webp})
# Squirrel (2009) is C++98 code.
set_target_properties(nyan_diesel_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 14)
target_include_directories(nyan_diesel_vendor
  PUBLIC ${_dv}/squirrel/include ${_dv}/zstd/lib ${_dv}/webp/src
  PRIVATE ${_dv}/squirrel/squirrel ${_dv}/webp)
target_compile_definitions(nyan_diesel_vendor PRIVATE
  ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0 ZSTD_NO_TRACE=1 NDEBUG)
# Vendored code: built as it is, not held to our warning level.
target_compile_options(nyan_diesel_vendor PRIVATE -w)
