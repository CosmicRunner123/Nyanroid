# nyan_core: platform-independent engine core shared by the desktop tools and the Android app.

find_package(ZLIB REQUIRED)

add_library(nyan_core STATIC
  src/common/engine.cpp
  src/common/error.cpp
  src/common/file_source.cpp
  src/common/sjis.cpp
  src/common/zlib_util.cpp
  src/common/xml.cpp
  src/audio/mixer.cpp
  src/media/mpeg_video.cpp
  src/media/ffmpeg_video.cpp
  src/cs2/blowfish.cpp
  src/cs2/mersenne_twister.cpp
  src/cs2/pe_resources.cpp
  src/cs2/vcodes.cpp
  src/cs2/kif_archive.cpp
  src/cs2/kif_seed_search.cpp
  src/cs2/asset_index.cpp
  src/cs2/hg_image.cpp
  src/cs2/cst_scene.cpp
  src/cs2/kcs_code.cpp
  src/cs2/dictionary.cpp
  src/cs2/scene_text.cpp
  src/cs2/expr.cpp
  src/cs2/message_text.cpp
  src/cs2/scene_player.cpp
  src/cs2/stage.cpp
  src/cs2/window_theme.cpp
  src/cs2/fes_script.cpp
  src/cs2/fes_runtime.cpp
  src/cs2/window_script.cpp
  src/cs2/game.cpp
  src/cs2/key_map.cpp
  src/cs2/save_data.cpp
  src/cs2/font_select.cpp
  src/cs2/anm.cpp
  src/cs2/kx2.cpp
  src/cs2/psb.cpp
  src/cs2/emote.cpp
  src/cs2/emote_profile.cpp
  src/cs2/emote_player.cpp
  src/cs2/emote_inertia.cpp
  src/cs2/particle.cpp
  src/cs2/cursor_image.cpp
  # White Album 2 (WA2)
  src/wa2/lzss.cpp
  src/wa2/kcap_archive.cpp
  src/wa2/lac_archive.cpp
  src/wa2/assets.cpp
  src/wa2/exe_info.cpp
  src/wa2/image.cpp
  src/wa2/script.cpp
  src/wa2/script_vm.cpp
  src/wa2/game.cpp
  src/wa2/natives_flow.cpp
  src/wa2/input.cpp
  src/wa2/system_data.cpp
  src/wa2/sound_out.cpp
  src/wa2/sound.cpp
  src/wa2/natives_sound.cpp
  src/wa2/subtitles.cpp
  src/wa2/gfx.cpp
  src/wa2/scene.cpp
  src/wa2/natives_scene.cpp
  src/wa2/text.cpp
  src/wa2/ani.cpp
  src/wa2/message.cpp
  src/wa2/natives_message.cpp
  src/wa2/effects.cpp
  src/wa2/natives_effects.cpp
  src/wa2/title.cpp
  src/wa2/save.cpp
  src/wa2/save_menu.cpp
  src/wa2/options.cpp
  src/wa2/music_room.cpp
  src/wa2/specials.cpp
  src/wa2/title_special.cpp
  src/wa2/voice_drama.cpp
  # Diesel Engine (Full Metal Daemon Muramasa, You and Me and Her)
  src/diesel/crypto.cpp
  src/diesel/npk_archive.cpp
  src/diesel/exe_info.cpp
  src/diesel/archives.cpp
  src/diesel/setting_ini.cpp
  src/diesel/image.cpp
  src/diesel/game_data.cpp
  src/diesel/natives_table.cpp
  src/diesel/natives_n1.cpp
  src/diesel/bytecode.cpp
  src/diesel/script_host.cpp
  src/diesel/natives_globals.cpp
  src/diesel/action.cpp
  src/diesel/natives_action.cpp
  src/diesel/save_store.cpp
  src/diesel/natives_file.cpp
  src/diesel/natives_math.cpp
  src/diesel/input.cpp
  src/diesel/system_config.cpp
  src/diesel/natives_app.cpp
  src/diesel/natives_camera.cpp
  src/diesel/natives_scene.cpp
  src/diesel/natives_particle.cpp
  src/diesel/particle.cpp
  src/diesel/random.cpp
  src/diesel/text_tokenizer.cpp
  src/diesel/natives_text.cpp
  src/diesel/tween.cpp
  src/diesel/sprite.cpp
  src/diesel/spline.cpp
  src/diesel/split.cpp
  src/diesel/sprite_geometry.cpp
  src/diesel/font.cpp
  src/diesel/text_layout.cpp
  src/diesel/sound_player.cpp
  src/diesel/natives_sprite.cpp
  src/diesel/ogg_info.cpp
  src/diesel/asf_info.cpp
  src/diesel/movie_clock.cpp
  src/diesel/movie_frames.cpp
  src/diesel/shake.cpp
  src/diesel/natives_sound.cpp
  src/diesel/natives_textobject.cpp
  src/diesel/natives_vmstate.cpp
  src/third_party_impl.cpp
)

target_include_directories(nyan_core
  PUBLIC include
  PRIVATE src ${PROJECT_SOURCE_DIR}/third_party)

target_link_libraries(nyan_core PUBLIC ZLIB::ZLIB)

# FFmpeg for Windows Media movies (cmake/ffmpeg.cmake: built from source, LGPL, static).
option(NYAN_WITH_FFMPEG "Build FFmpeg for movies" ON)
if(NYAN_WITH_FFMPEG)
  include(${PROJECT_SOURCE_DIR}/cmake/ffmpeg.cmake)
  target_link_libraries(nyan_core PRIVATE nyan_ffmpeg)
  set_source_files_properties(src/media/ffmpeg_video.cpp PROPERTIES COMPILE_DEFINITIONS NYAN_HAVE_FFMPEG)
endif()
# Diesel Engine's vendored libraries (Squirrel 2.2.4, zstd's and libwebp's decoders): src/diesel/vendor.
include(src/diesel/vendor/vendor.cmake)
target_link_libraries(nyan_core PRIVATE nyan_diesel_vendor)
target_compile_features(nyan_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nyan_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# Diesel: the curves lifted from the exe and the random floats keep its float operations (no fused
# multiply-add).
set_source_files_properties(src/diesel/random.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_source_files_properties(src/diesel/action.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")
# CatSystem2's particle plugins: the DLLs' SSE float operations, unfused (clang fuses multiply-adds
# on arm64 by default).
set_source_files_properties(src/cs2/particle.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
# Vendored single-header libraries: don't hold them to our warning level.
set_source_files_properties(src/third_party_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# Diesel's reverb: XAudio2's own DSP when the private companion has its port (not publishable),
# else FAudio's re-implementation (src/diesel/faudio_reverb.inc).
set(NYAN_XAUDIO2_REVERB "${PROJECT_SOURCE_DIR}/private/diesel/xaudio2_reverb.inc")
if(EXISTS "${NYAN_XAUDIO2_REVERB}")
  set_property(SOURCE src/diesel/sound_player.cpp APPEND PROPERTY COMPILE_DEFINITIONS
               "NYAN_XAUDIO2_REVERB=\"${NYAN_XAUDIO2_REVERB}\"")
endif()
