cmake_minimum_required(VERSION 3.14)
project(emu68-rpivid-codec VERSION 0.5 LANGUAGES C ASM)
# The one version of the repository: every binary takes it.  Bump it here only.

# rpivid.codec: an H.265 backend of videocodec.library on the Raspberry Pi 4's
# HEVC block (docs/).  A component of the emu68 driver stack, which builds it
# with its toolchain and installs it into the shared prefix where the packages
# below come from.
#
#   src/, include/   rpivid.codec (LIBS:videocodec/): docs/architecture.md section 3.1 names the files
#   tests/           its test suite (developer-guide.md section 4), on the host and on the Amiga

string(TIMESTAMP CURRENT_DATE "(%d.%m.%Y)")
if(EMU68_BUILD_STAMP)
    string(APPEND CURRENT_DATE " ${EMU68_BUILD_STAMP}")
endif()

# Dependencies, all from the stack's install prefix: emu68-common (devtree, MMIO,
# timing, dma_mem, the driver task, cache ops), gic400.library's protos (the
# block's interrupt), and the vchiq library's VideoCodec package (the backend ABI
# headers).
find_package(Emu68Common 2.3 CONFIG REQUIRED)
find_package(GIC400 1.3 CONFIG REQUIRED)
find_package(VideoCodec 0.6 CONFIG REQUIRED) # 0.6: vchiqtest, the suite's harness, in the install tree

option(EMU68_LTO "Link-time optimization for the backend module" ON)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# Debug-output backend defines (EMU68_DEBUG_BACKEND) and the cache_ops.h
# LVO-fallback define (EMU68_FORCE_LVO_CACHE_OPS), as the stack forwards them.
emu68_debug_definitions()
emu68_cache_ops_definitions()

# rpivid.codec, the backend module: freestanding, no writable section (the
# stack's module layout script checks), LTO.  Its function table is the backend
# ABI; it exports no header of its own.
set(RPIVID_VERSTRING "$VER: rpivid.codec ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} ${CURRENT_DATE}")
set(RPIVID_MODULE_COMPILE_OPTIONS
    -O3
    -ffreestanding
    -Wall
    -Wextra
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wmissing-prototypes
    -Wstrict-prototypes
)
set(RPIVID_MODULE_LINK_OPTIONS
    -nostdlib
    -nostartfiles
    -Wall
    -Wextra
    -s
)

add_executable(rpivid_codec
    src/rpivid_lib.c
    src/rpivid_backend.c
    src/rpivid_codecs.c
    src/session.c
    src/session_objects.c
    src/session_parse.c
    src/cmdgen/hevc_d_h265.c
    src/stream/bits.c
    src/stream/nal.c
    src/stream/ps.c
    src/stream/slice.c
    src/stream/rps.c
    src/stream/dpb.c
    src/stream/record.c
    src/stream/stream.c
    src/narrow/narrow.S
    src/hw/hw.c
    src/hw/hw_task.c
    src/hw/hw_isr.c
)
target_compile_definitions(rpivid_codec PRIVATE
    LIBRARY_IDSTRING="${RPIVID_VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="rpivid.codec"
)
target_compile_options(rpivid_codec PRIVATE ${RPIVID_MODULE_COMPILE_OPTIONS})
target_link_options(rpivid_codec PRIVATE ${RPIVID_MODULE_LINK_OPTIONS})
target_include_directories(rpivid_codec PRIVATE include src src/cmdgen) # not src/stream: its bits.h would shadow emu68-common's
target_link_libraries(rpivid_codec PRIVATE VideoCodec::headers Emu68Common::common)

emu68_module_layout(rpivid_codec)
# The interrupt server answers with the Z flag; emu68_isr_z_check() slices it out
# of the linked module by its section.  hw_isr.c is file-scope asm: no IR for LTO.
emu68_enable_lto(rpivid_codec)
emu68_lto_keep_real_objects(rpivid_codec
    src/hw/hw_isr.c
)
emu68_isr_z_check(rpivid_codec SERVERS rp_hw_isr)

set_target_properties(rpivid_codec PROPERTIES
    OUTPUT_NAME rpivid
    SUFFIX ".codec"
)

# A backend lives in LIBS:videocodec/, where videocodec.library looks for it.
install(TARGETS rpivid_codec RUNTIME DESTINATION LIBS/videocodec)
