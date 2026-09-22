cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.3
)
# Defines the INTERFACE target `ArduinoJson` (header-only include dir).
FetchContent_MakeAvailable(arduinojson)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

# ── Font backend test variants (design D2/D9) ─────────────────────────────
# Native-TTF suites build as two variants: the stb default and a FreeType
# variant compiled with CROSSPOINT_FONT_BACKEND_FT=1. Each backend is
# asserted against its own invariants — no cross-backend metric equality.
set(FREEINK_FT_ROOT "${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont")
set(FONT_FT_TEST_SOURCES
  "${FREEINK_FT_ROOT}/src/FtFont.cpp"
  "${FREEINK_FT_ROOT}/src/Gsub.cpp"
  "${FREEINK_FT_ROOT}/src/Gpos.cpp"
  "${FREEINK_FT_ROOT}/src/FontAlloc.c"
)
file(GLOB FONT_FT_WRAPPER_SOURCES "${FREEINK_FT_ROOT}/src/freetype/*.c")
list(APPEND FONT_FT_TEST_SOURCES ${FONT_FT_WRAPPER_SOURCES})
set(FONT_FT_TEST_INCLUDES
  "${FREEINK_FT_ROOT}/freetype-config/include"
  "${FREEINK_FT_ROOT}/third_party/freetype/include"
)

# Adds TARGET (stb backend) and TARGET BackendFT (FreeType backend): same
# sources/includes/definitions, the FT variant additionally compiling
# FtFont.cpp + the vendored FreeType amalgam under the backend flag.
function(add_font_backend_tests TARGET)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;DEFINITIONS;LIBS" ${ARGN})
  add_executable(${TARGET} ${ARG_SOURCES})
  target_include_directories(${TARGET} PRIVATE ${ARG_INCLUDES})
  target_compile_definitions(${TARGET} PRIVATE ${ARG_DEFINITIONS})
  target_link_libraries(${TARGET} PRIVATE crosspoint_test_common GTest::gtest_main ${ARG_LIBS})
  gtest_discover_tests(${TARGET})

  add_executable(${TARGET}BackendFT ${ARG_SOURCES} ${FONT_FT_TEST_SOURCES})
  target_compile_definitions(${TARGET}BackendFT PRIVATE ${ARG_DEFINITIONS} CROSSPOINT_FONT_BACKEND_FT=1)
  target_include_directories(${TARGET}BackendFT PRIVATE ${ARG_INCLUDES} ${FONT_FT_TEST_INCLUDES})
  target_link_libraries(${TARGET}BackendFT PRIVATE crosspoint_test_common GTest::gtest_main ${ARG_LIBS})
  gtest_discover_tests(${TARGET}BackendFT)
endfunction()

add_subdirectory(ota_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(reading_stats)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(section_incremental)
add_subdirectory(dictionary_selection)
add_subdirectory(gray_planes)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(home_button)
add_subdirectory(quick_page_capture)

add_subdirectory(inflate_stream)
add_subdirectory(chunk_coalescer)
add_subdirectory(fs_helpers)
add_subdirectory(firmware_flasher)
add_subdirectory(absolute_grayscale)
add_subdirectory(book_font_loader)
add_subdirectory(sd_card_cache_storage)
add_subdirectory(sd_card_font)
add_subdirectory(psram_image_staging)
add_subdirectory(ttf_reader_runtime)
add_subdirectory(progress_manager)
add_subdirectory(style_bits)
add_subdirectory(page_paint)
add_subdirectory(ttf_word_select)
add_subdirectory(chapter_index_engine)
add_subdirectory(fibp_prefetch_policy)
add_subdirectory(font_backend)
add_subdirectory(ui_app_handlers)
add_subdirectory(activity_result)
add_subdirectory(glyph_bitmap)
