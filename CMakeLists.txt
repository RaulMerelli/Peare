cmake_minimum_required(VERSION 3.16)

include(cmake/CheckRepositoryRoot.cmake)

project(Peare VERSION 1.0.0 LANGUAGES C CXX)


# Peare sources retain a C++11 baseline on Qt 5 desktop targets.
set(PEARE_CXX_STANDARD "AUTO" CACHE STRING "C++ standard: AUTO, 11 or 17")
set_property(CACHE PEARE_CXX_STANDARD PROPERTY STRINGS AUTO 11 17)
set(CMAKE_AUTORCC ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Keep MSVC import libraries and .exp files out of the runtime directory.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set("CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

# Platform policy:
#   - Windows and Linux: Qt 5.15 (the supplied desktop scripts use 5.15.2)
#   - Android and macOS: Qt 6
# AUTO follows that policy. Explicit overrides remain available for platform
# build scripts, but Qt 6 is intentionally rejected on Windows and Linux.
set(PEARE_QT_MAJOR "AUTO" CACHE STRING "Qt major version: AUTO, 5 or 6")
set_property(CACHE PEARE_QT_MAJOR PROPERTY STRINGS AUTO 5 6)

if(PEARE_QT_MAJOR STREQUAL "AUTO")
    if(ANDROID OR APPLE)
        set(PEARE_EFFECTIVE_QT_MAJOR 6)
    else()
        set(PEARE_EFFECTIVE_QT_MAJOR 5)
    endif()
elseif(PEARE_QT_MAJOR STREQUAL "5" OR PEARE_QT_MAJOR STREQUAL "6")
    set(PEARE_EFFECTIVE_QT_MAJOR ${PEARE_QT_MAJOR})
else()
    message(FATAL_ERROR "PEARE_QT_MAJOR must be AUTO, 5 or 6")
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6" AND NOT (ANDROID OR APPLE))
    message(FATAL_ERROR "Qt 6 is reserved for Android and macOS; Windows and Linux use Qt 5.15")
endif()
if(ANDROID AND NOT PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6")
    message(FATAL_ERROR "Android builds require Qt 6")
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "5" AND CMAKE_CROSSCOMPILING)
    set(PEARE_QT_HOST_PATH "" CACHE PATH "Host Qt tools used while cross-compiling Qt 5")
    if(NOT PEARE_QT_HOST_PATH)
        message(FATAL_ERROR "PEARE_QT_HOST_PATH is required for a Qt 5 cross-build")
    endif()

    foreach(qt_tool qmake moc rcc uic)
        set(qt_tool_path "${PEARE_QT_HOST_PATH}/bin/${qt_tool}")
        if(NOT EXISTS "${qt_tool_path}")
            message(FATAL_ERROR "Missing host Qt tool: ${qt_tool_path}")
        endif()
        if(NOT TARGET Qt5::${qt_tool})
            add_executable(Qt5::${qt_tool} IMPORTED GLOBAL)
            set_target_properties(Qt5::${qt_tool} PROPERTIES IMPORTED_LOCATION "${qt_tool_path}")
        endif()
    endforeach()
endif()

if(PEARE_EFFECTIVE_QT_MAJOR STREQUAL "6")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Core5Compat)
    set(PEARE_QT_PACKAGE Qt6)
    set(QT_VERSION_MAJOR 6)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)
    set(PEARE_QT_PACKAGE Qt5)
    set(QT_VERSION_MAJOR 5)
endif()

if(PEARE_CXX_STANDARD STREQUAL "AUTO")
    if(PEARE_QT_MAJOR STREQUAL "6")
        set(PEARE_EFFECTIVE_CXX_STANDARD 17)
    else()
        set(PEARE_EFFECTIVE_CXX_STANDARD 11)
    endif()
elseif(PEARE_CXX_STANDARD STREQUAL "11" OR PEARE_CXX_STANDARD STREQUAL "17")
    set(PEARE_EFFECTIVE_CXX_STANDARD ${PEARE_CXX_STANDARD})
else()
    message(FATAL_ERROR "Peare requires PEARE_CXX_STANDARD=AUTO, 11 or 17")
endif()
set(CMAKE_CXX_STANDARD ${PEARE_EFFECTIVE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Treat sources as UTF-8 on MSVC too. Without this, non-ASCII characters in string
# literals (e.g. the "—" in partition names) are read in the system codepage and
# mangled into mojibake at runtime. GCC/Clang already default to UTF-8.
if(MSVC)
    add_compile_options(/utf-8)
endif()


set(PEARE_QT_CORE ${PEARE_QT_PACKAGE}::Core)
set(PEARE_QT_GUI ${PEARE_QT_PACKAGE}::Gui)
set(PEARE_QT_WIDGETS ${PEARE_QT_PACKAGE}::Widgets)
if(QT_VERSION_MAJOR EQUAL 6)
    set(PEARE_QT_CORE5COMPAT Qt6::Core5Compat)
else()
    set(PEARE_QT_CORE5COMPAT)
endif()

message(STATUS "Peare Qt profile: Qt ${QT_VERSION_MAJOR}")
message(STATUS "Peare source baseline: C++11; compiler mode: C++${PEARE_EFFECTIVE_CXX_STANDARD}")

if(WIN32)
    message(STATUS "Peare targets: Peare.exe + PeareOpener.dll + PeareDecoder.dll + PeareWriter.dll")
elseif(APPLE)
    message(STATUS "Peare targets: Peare + libPeareOpener + libPeareDecoder + libPeareWriter")
else()
    message(STATUS "Peare targets: Peare + libPeareOpener + libPeareDecoder + libPeareWriter")
endif()

set(PEARE_FONT_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/font/third_party")
file(GLOB_RECURSE PEARE_BROTLI_COMMON_SOURCES CONFIGURE_DEPENDS
    "${PEARE_FONT_THIRD_PARTY_DIR}/brotli/c/common/*.c")
file(GLOB_RECURSE PEARE_BROTLI_DECODER_SOURCES CONFIGURE_DEPENDS
    "${PEARE_FONT_THIRD_PARTY_DIR}/brotli/c/dec/*.c")
add_library(PeareBrotliDecoder STATIC
    ${PEARE_BROTLI_COMMON_SOURCES}
    ${PEARE_BROTLI_DECODER_SOURCES}
)
target_include_directories(PeareBrotliDecoder PUBLIC
    "${PEARE_FONT_THIRD_PARTY_DIR}/brotli/c/include")

add_library(PeareWoff2Decoder STATIC
    src/decoder/font/third_party/woff2/src/table_tags.cc
    src/decoder/font/third_party/woff2/src/variable_length.cc
    src/decoder/font/third_party/woff2/src/woff2_common.cc
    src/decoder/font/third_party/woff2/src/woff2_dec.cc
    src/decoder/font/third_party/woff2/src/woff2_out.cc
)
target_compile_features(PeareWoff2Decoder PRIVATE cxx_std_11)
target_include_directories(PeareWoff2Decoder
    PUBLIC "${PEARE_FONT_THIRD_PARTY_DIR}/woff2/include"
    PRIVATE "${PEARE_FONT_THIRD_PARTY_DIR}/woff2/src")
target_link_libraries(PeareWoff2Decoder PRIVATE PeareBrotliDecoder)

set(PEARE_LIBEOT_DIR "${PEARE_FONT_THIRD_PARTY_DIR}/libeot")
add_library(PeareMtxDecoder STATIC
    ${PEARE_LIBEOT_DIR}/src/EOT.c
    ${PEARE_LIBEOT_DIR}/src/libeot.c
    ${PEARE_LIBEOT_DIR}/src/triplet_encodings.c
    ${PEARE_LIBEOT_DIR}/src/writeFontFile.c
    ${PEARE_LIBEOT_DIR}/src/ctf/parseCTF.c
    ${PEARE_LIBEOT_DIR}/src/ctf/parseTTF.c
    ${PEARE_LIBEOT_DIR}/src/ctf/SFNTContainer.c
    ${PEARE_LIBEOT_DIR}/src/lzcomp/ahuff.c
    ${PEARE_LIBEOT_DIR}/src/lzcomp/bitio.c
    ${PEARE_LIBEOT_DIR}/src/lzcomp/liblzcomp.c
    ${PEARE_LIBEOT_DIR}/src/lzcomp/lzcomp.c
    ${PEARE_LIBEOT_DIR}/src/lzcomp/mtxmem.c
    ${PEARE_LIBEOT_DIR}/src/util/stream.c
    ${PEARE_LIBEOT_DIR}/src/peare_mtx_bridge.c
)
set_target_properties(PeareMtxDecoder PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
)
target_compile_definitions(PeareMtxDecoder PRIVATE DECOMPRESS_ON)
target_include_directories(PeareMtxDecoder PRIVATE
    ${PEARE_LIBEOT_DIR}/inc
    ${PEARE_LIBEOT_DIR}/src
    ${PEARE_LIBEOT_DIR}/src/lzcomp
    ${PEARE_LIBEOT_DIR}/src/util
)

# Peare-owned compression decoders used by archive and disk readers.
add_library(PeareOpener SHARED
    src/common/compression/UnixCompress.h
    src/common/compression/UnixCompress.cpp
    src/opener/compression/ArjGost40.h
    src/opener/compression/ArjGost40.cpp
    src/opener/compression/PackMp3Decoder.h
    src/opener/compression/PackMp3Decoder.cpp
    src/opener/compression/third_party/packmp3/packmp3.cpp
    src/opener/compression/third_party/packmp3/bitops.cpp
    src/opener/compression/third_party/packmp3/bitops.h
    src/opener/compression/third_party/packmp3/aricoder.cpp
    src/opener/compression/third_party/packmp3/aricoder.h
    src/opener/compression/third_party/packmp3/huffmp3.cpp
    src/opener/compression/third_party/packmp3/huffmp3.h
    src/opener/compression/third_party/packmp3/huffmp3tbl.h
    src/opener/compression/third_party/packmp3/pmp3bitlen.h
    src/opener/compression/third_party/packmp3/pmp3tbl.h
    src/opener/compression/third_party/packmp3/packmp3lib.h
    src/opener/compression/WinZipJpegDecoder.h
    src/opener/compression/WinZipJpegDecoder.cpp
    src/opener/compression/third_party/winzipjpeg/ArithmeticDecoder.c
    src/opener/compression/third_party/winzipjpeg/ArithmeticDecoder.h
    src/opener/compression/third_party/winzipjpeg/ClangAnalyser.h
    src/opener/compression/third_party/winzipjpeg/Decompressor.c
    src/opener/compression/third_party/winzipjpeg/Decompressor.h
    src/opener/compression/third_party/winzipjpeg/InputStream.h
    src/opener/compression/third_party/winzipjpeg/JPEG.c
    src/opener/compression/third_party/winzipjpeg/JPEG.h
    src/opener/compression/third_party/winzipjpeg/LZMA.h
    src/opener/compression/WavPackDecoder.h
    src/opener/compression/WavPackDecoder.cpp
    src/opener/compression/third_party/wavpack/include/wavpack.h
    src/opener/compression/third_party/wavpack/src/common_utils.c
    src/opener/compression/third_party/wavpack/src/decorr_utils.c
    src/opener/compression/third_party/wavpack/src/entropy_utils.c
    src/opener/compression/third_party/wavpack/src/extra1.c
    src/opener/compression/third_party/wavpack/src/extra2.c
    src/opener/compression/third_party/wavpack/src/open_utils.c
    src/opener/compression/third_party/wavpack/src/open_filename.c
    src/opener/compression/third_party/wavpack/src/open_legacy.c
    src/opener/compression/third_party/wavpack/src/open_raw.c
    src/opener/compression/third_party/wavpack/src/pack.c
    src/opener/compression/third_party/wavpack/src/pack_dns.c
    src/opener/compression/third_party/wavpack/src/pack_floats.c
    src/opener/compression/third_party/wavpack/src/pack_utils.c
    src/opener/compression/third_party/wavpack/src/read_words.c
    src/opener/compression/third_party/wavpack/src/tags.c
    src/opener/compression/third_party/wavpack/src/tag_utils.c
    src/opener/compression/third_party/wavpack/src/unpack.c
    src/opener/compression/third_party/wavpack/src/unpack_floats.c
    src/opener/compression/third_party/wavpack/src/unpack_seek.c
    src/opener/compression/third_party/wavpack/src/unpack_utils.c
    src/opener/compression/third_party/wavpack/src/write_words.c
    src/opener/compression/third_party/wavpack/src/decorr_tables.h
    src/opener/compression/third_party/wavpack/src/unpack3.h
    src/opener/compression/third_party/wavpack/src/wavpack_local.h
    src/opener/compression/third_party/wavpack/src/wavpack_version.h
    src/opener/compression/peare/Bzip2Decoder.h
    src/opener/compression/peare/Bzip2Decoder.cpp
    src/opener/compression/LzmsDecoder.h
    src/opener/compression/LzmsDecoder.cpp
    src/opener/compression/third_party/wimlib/src/lzms_decompress.c
    src/opener/compression/third_party/wimlib/src/lzms_common.c
    src/opener/compression/third_party/wimlib/src/decompress_common.c
    src/opener/compression/third_party/wimlib/src/compress_common.c
    src/common/rar/Rar4PpmdDecoder.h
    src/common/rar/Rar4PpmdDecoder.cpp
    src/common/rar/Rar4Crypto.h
    src/common/rar/Rar4Crypto.cpp
    src/common/rar/Rar4Vm.h
    src/common/rar/Rar4Vm.cpp
    src/common/compression/SevenZipBcj2Decoder.h
    src/common/compression/SevenZipBcj2Decoder.cpp
    src/common/compression/SevenZipAesDecoder.h
    src/common/compression/SevenZipAesDecoder.cpp
    src/opener/compression/LzmaDecoder.h
    src/opener/compression/LzmaDecoder.cpp
    src/opener/compression/third_party/blast/blast.h
    src/opener/compression/third_party/blast/blast.c
    src/opener/compression/third_party/zstd/zstd.h
    src/opener/compression/third_party/zstd/zstd_errors.h
    src/opener/compression/third_party/zstd/common/debug.c
    src/opener/compression/third_party/zstd/common/entropy_common.c
    src/opener/compression/third_party/zstd/common/error_private.c
    src/opener/compression/third_party/zstd/common/fse_decompress.c
    src/opener/compression/third_party/zstd/common/pool.c
    src/opener/compression/third_party/zstd/common/threading.c
    src/opener/compression/third_party/zstd/common/xxhash.c
    src/opener/compression/third_party/zstd/common/zstd_common.c
    src/opener/compression/third_party/zstd/decompress/huf_decompress.c
    src/opener/compression/third_party/zstd/decompress/zstd_ddict.c
    src/opener/compression/third_party/zstd/decompress/zstd_decompress_block.c
    src/opener/compression/third_party/zstd/decompress/zstd_decompress.c
    src/opener/compression/third_party/lzma/Compiler.h
    src/opener/compression/third_party/lzma/Precomp.h
    src/opener/compression/third_party/lzma/7zTypes.h
    src/opener/compression/third_party/lzma/Aes.h
    src/opener/compression/third_party/lzma/Aes.c
    src/opener/compression/third_party/lzma/Sha256.h
    src/opener/compression/third_party/lzma/Sha256.c
    src/opener/compression/third_party/lzma/LzmaDec.h
    src/opener/compression/third_party/lzma/LzmaDec.c
    src/opener/compression/third_party/lzma/Lzma2Dec.h
    src/opener/compression/third_party/lzma/Lzma2Dec.c
    src/opener/compression/third_party/lzma/Ppmd.h
    src/opener/compression/third_party/lzma/Ppmd7.h
    src/opener/compression/third_party/lzma/Ppmd7.c
    src/opener/compression/third_party/lzma/Ppmd7Dec.c
    src/opener/compression/third_party/lzma/Ppmd8.h
    src/opener/compression/third_party/lzma/Ppmd8.c
    src/opener/compression/third_party/lzma/Ppmd8Dec.c
    src/opener/compression/third_party/lzma/CpuArch.h
    src/opener/compression/third_party/lzma/Bcj2.h
    src/opener/compression/third_party/lzma/Bcj2.c
    src/opener/compression/third_party/lzma/Bra.h
    src/opener/compression/third_party/lzma/Bra.c
    src/opener/compression/third_party/lzma/Bra86.c
    src/opener/compression/third_party/lzma/RotateDefs.h
    src/opener/compression/third_party/lzma/Delta.h
    src/opener/compression/third_party/lzma/Delta.c
    src/opener/compression/peare/LhaDecoder.h
    src/opener/compression/peare/LhaDecoder.cpp
    src/opener/compression/peare/LhaLh23Decoder.h
    src/opener/compression/peare/LhaLh23Decoder.cpp
    src/opener/compression/peare/LhaLk7Decoder.h
    src/opener/compression/peare/LhaLk7Decoder.cpp
    src/opener/compression/QuantumCabDecoder.h
    src/opener/compression/QuantumCabDecoder.cpp
    src/opener/compression/peare/LhaPm1Decoder.h
    src/opener/compression/peare/LhaPm1Decoder.cpp
    src/opener/compression/peare/LhaPm2Decoder.h
    src/opener/compression/peare/LhaPm2Decoder.cpp
    src/opener/api/peare/peare_types.h
    src/opener/api/peare/peare_opener.h
    src/opener/api/Api.cpp
    src/opener/compression/peare/lzx_frontends.h
    src/opener/compression/peare/LzxDecoder.cpp
    src/opener/api/PeareResourceSnapshot.h
    src/opener/OpenerSession.h
    src/opener/OpenerSession.cpp
    src/opener/modules/Module.h
    src/opener/modules/Compat.h
    src/opener/modules/ModuleFactory.h
    src/opener/modules/ModuleFactory.cpp
    src/opener/modules/ModuleFormat.h
    src/opener/modules/ModuleFormat.cpp
    src/opener/modules/PeModule.h
    src/opener/modules/PeModuleBase.h
    src/opener/modules/PeModuleCommon.h
    src/opener/modules/PeModuleCommon.cpp
    src/opener/PeImageExport.h
    src/opener/PeImageExport.cpp
    src/opener/modules/PeFileModule.h
    src/opener/modules/PeFileModule.cpp
    src/opener/modules/PeImageModule.h
    src/opener/modules/PeImageModule.cpp
    src/opener/modules/XexModule.h
    src/opener/modules/XexModule.cpp
    src/opener/modules/Crypto.h
    src/opener/modules/Crypto.cpp
    src/opener/modules/XbeModule.h
    src/opener/modules/XbeModule.cpp
    src/opener/modules/XuizModule.h
    src/opener/modules/XuizModule.cpp
    src/opener/modules/LivePirsModule.h
    src/opener/modules/LivePirsModule.cpp
    src/opener/modules/ConModule.h
    src/opener/modules/ConModule.cpp
    src/opener/modules/Os2PackModule.h
    src/opener/modules/Os2PackModule.cpp
    src/opener/modules/Os2EaModule.h
    src/opener/modules/Os2EaModule.cpp
    src/opener/modules/Os2Pack2Decoder.h
    src/opener/modules/Os2Pack2Decoder.cpp
    src/opener/modules/Os2Pack2Tables.h
    src/opener/modules/SzddModule.h
    src/opener/modules/SzddModule.cpp
    src/opener/modules/KwajModule.h
    src/opener/modules/KwajModule.cpp
    src/opener/modules/XbcompressModule.h
    src/opener/modules/XbcompressModule.cpp
    src/opener/modules/NsisModule.h
    src/opener/modules/NsisModule.cpp
    src/opener/modules/GzipModule.h
    src/opener/modules/GzipModule.cpp
    src/opener/modules/XzModule.h
    src/opener/modules/XzModule.cpp
    src/opener/modules/LzipModule.h
    src/opener/modules/LzipModule.cpp
    src/opener/modules/LzmaModule.h
    src/opener/modules/LzmaModule.cpp
    src/opener/modules/ZstdModule.h
    src/opener/modules/ZstdModule.cpp
    src/opener/modules/Bzip2Module.h
    src/opener/modules/Bzip2Module.cpp
    src/opener/modules/ArModule.h
    src/opener/modules/ArModule.cpp
    src/opener/modules/DebModule.h
    src/opener/modules/DebModule.cpp
    src/opener/modules/RpmModule.h
    src/opener/modules/RpmModule.cpp
    src/opener/modules/ChmModule.h
    src/opener/modules/ChmModule.cpp
    src/opener/modules/ArjModule.h
    src/opener/modules/ArjModule.cpp
    src/opener/modules/XarModule.h
    src/opener/modules/XarModule.cpp
    src/opener/modules/CpioModule.cpp
    src/opener/modules/UnixCompressModule.cpp
    src/opener/modules/IhexModule.h
    src/opener/modules/IhexModule.cpp
    src/opener/modules/LhaModule.h
    src/opener/modules/LhaModule.cpp
    src/opener/modules/SevenZipModule.h
    src/opener/modules/SevenZipModule.cpp
    src/opener/modules/Rar4Module.h
    src/opener/modules/Rar4Module.cpp
    src/opener/modules/Rar5Module.h
    src/opener/modules/Rar5Module.cpp
    src/common/rar/Rar4LzDecoder.h
    src/common/rar/Rar4LzDecoder.cpp
    src/common/rar/Rar5Crypto.h
    src/common/rar/Rar5Crypto.cpp
    src/opener/modules/UuencodeModule.h
    src/opener/modules/UuencodeModule.cpp
    src/opener/modules/ResxModule.h
    src/opener/modules/ResxModule.cpp
    src/opener/modules/CueBinModule.h
    src/opener/modules/CueBinModule.cpp
    src/opener/modules/SiemensImgModule.h
    src/opener/modules/SiemensImgModule.cpp
    src/opener/modules/SiemensAdonisModule.h
    src/opener/modules/SiemensAdonisModule.cpp
    src/opener/modules/SiemensFwfModule.h
    src/opener/modules/SiemensFwfModule.cpp
    src/opener/modules/SiemensFsfModule.h
    src/opener/modules/SiemensFsfModule.cpp
    src/opener/modules/wince/WinceRomModule.h
    src/opener/modules/wince/WinceRomModule.cpp
    src/opener/modules/wince/WinceCompression.h
    src/opener/modules/wince/WinceCompression.cpp
    src/common/compression/DeflateDecoder.h
    src/common/compression/DeflateDecoder.cpp
    src/opener/modules/SiemensImgParser.h
    src/opener/modules/SiemensFwfParser.h
    src/opener/modules/stfs/STFSCommon.h
    src/opener/modules/stfs/STFSCommon.cpp
    src/opener/modules/NeModule.h
    src/opener/modules/NeModule.cpp
    src/opener/modules/LxModule.h
    src/opener/modules/LxModule.cpp
    src/opener/modules/LeModule.h
    src/opener/modules/LeModule.cpp
    src/opener/modules/PeResources.h
    src/opener/modules/PeResources.cpp
    src/opener/fs/DiscStore.h
    src/opener/fs/AndroidSparseStore.h
    src/opener/fs/AndroidSparseStore.cpp
    src/opener/fs/AndroidSuperStore.h
    src/opener/fs/AndroidSuperStore.cpp
    src/opener/fs/BlockDecompressor.h
    src/opener/fs/DiscFileSystem.h
    src/opener/fs/Iso9660Reader.h
    src/opener/fs/Iso9660Reader.cpp
    src/opener/fs/BlockDecompressor.h
    src/opener/fs/XpressDecoder.cpp
    src/opener/fs/WimReader.h
    src/opener/fs/WimReader.cpp
    src/opener/fs/FatReader.h
    src/opener/fs/FatReader.cpp
    src/opener/fs/AmigaFsReader.h
    src/opener/fs/AmigaFsReader.cpp
    src/opener/fs/FloppyImage.h
    src/opener/fs/FloppyImage.cpp
    src/opener/fs/UdfReader.h
    src/opener/fs/UdfReader.cpp
    src/opener/fs/UfsReader.h
    src/opener/fs/UfsReader.cpp
    src/opener/fs/ExFatReader.h
    src/opener/fs/ExFatReader.cpp
    src/opener/fs/VmdkDisk.h
    src/opener/fs/VmdkDisk.cpp
    src/opener/fs/VhdDisk.h
    src/opener/fs/VhdDisk.cpp
    src/opener/fs/VdiDisk.h
    src/opener/fs/VdiDisk.cpp
    src/opener/fs/QcowDisk.h
    src/opener/fs/QcowDisk.cpp
    src/opener/fs/ParallelsDisk.h
    src/opener/fs/ParallelsDisk.cpp
    src/opener/fs/VhdxDisk.h
    src/opener/fs/VhdxDisk.cpp
    src/opener/fs/PartitionTable.h
    src/opener/fs/PartitionTable.cpp
    src/opener/fs/LinuxRaid.h
    src/opener/fs/LinuxRaid.cpp
    src/opener/fs/DynamicDisk.h
    src/opener/fs/DynamicDisk.cpp
    src/opener/fs/ExtReader.h
    src/opener/fs/ExtReader.cpp
    src/opener/fs/NtfsReader.h
    src/opener/fs/NtfsReader.cpp
    src/opener/fs/XfsReader.h
    src/opener/fs/XfsReader.cpp
    src/opener/fs/JfsReader.h
    src/opener/fs/JfsReader.cpp
    src/opener/fs/HpfsReader.h
    src/opener/fs/HpfsReader.cpp
    src/opener/fs/SquashFsReader.h
    src/opener/fs/SquashFsReader.cpp
    src/opener/fs/CramFsReader.h
    src/opener/fs/CramFsReader.cpp
    src/opener/fs/HfsPlusReader.h
    src/opener/fs/HfsPlusReader.cpp
    src/opener/fs/DmgDisk.h
    src/opener/fs/DmgDisk.cpp
    src/opener/fs/BtrfsReader.h
    src/opener/fs/BtrfsReader.cpp
    src/opener/fs/RegistryHiveReader.h
    src/opener/fs/RegistryHiveReader.cpp
    src/opener/modules/FsLevel.h
    src/opener/modules/IsoModule.h
    src/opener/modules/IsoModule.cpp
    src/opener/modules/WimModule.h
    src/opener/modules/WimModule.cpp
    src/opener/modules/FatModule.h
    src/opener/modules/FatModule.cpp
    src/opener/modules/UdfModule.h
    src/opener/modules/UdfModule.cpp
    src/opener/modules/UfsModule.h
    src/opener/modules/UfsModule.cpp
    src/opener/modules/ExFatModule.h
    src/opener/modules/ExFatModule.cpp
    src/opener/modules/VmdkModule.h
    src/opener/modules/VmdkModule.cpp
    src/opener/modules/VhdModule.h
    src/opener/modules/VhdModule.cpp
    src/opener/modules/VdiModule.h
    src/opener/modules/VdiModule.cpp
    src/opener/modules/QcowModule.h
    src/opener/modules/QcowModule.cpp
    src/opener/modules/ParallelsModule.h
    src/opener/modules/ParallelsModule.cpp
    src/opener/modules/Ps2RomdirParser.h
    src/opener/modules/Ps2RomdirParser.cpp
    src/opener/modules/Ps2RomdirModule.h
    src/opener/modules/Ps2RomdirModule.cpp
    src/opener/modules/OleCompound.h
    src/opener/modules/OleCompound.cpp
    src/opener/modules/MsiModule.h
    src/opener/modules/MsiModule.cpp
    src/opener/modules/ResourceForkModule.h
    src/opener/modules/ResourceForkModule.cpp
    src/opener/modules/MsgModule.h
    src/opener/modules/MsgModule.cpp
    src/opener/modules/VhdxModule.h
    src/opener/modules/VhdxModule.cpp
    src/opener/modules/SdiModule.h
    src/opener/modules/SdiModule.cpp
    src/opener/modules/XvaModule.h
    src/opener/modules/XvaModule.cpp
    src/opener/modules/SwapModule.h
    src/opener/modules/SwapModule.cpp
    src/opener/modules/LvmModule.h
    src/opener/modules/LvmModule.cpp
    src/opener/modules/LinuxRaidModule.h
    src/opener/modules/LinuxRaidModule.cpp
    src/opener/modules/DynamicDiskModule.h
    src/opener/modules/DynamicDiskModule.cpp
    src/opener/modules/ExtModule.h
    src/opener/modules/ExtModule.cpp
    src/opener/modules/NtfsModule.h
    src/opener/modules/NtfsModule.cpp
    src/opener/modules/XfsModule.h
    src/opener/modules/XfsModule.cpp
    src/opener/modules/JfsModule.h
    src/opener/modules/JfsModule.cpp
    src/opener/modules/HpfsModule.h
    src/opener/modules/HpfsModule.cpp
    src/opener/modules/SquashFsModule.h
    src/opener/modules/SquashFsModule.cpp
    src/opener/modules/CramFsModule.h
    src/opener/modules/CramFsModule.cpp
    src/opener/modules/HfsPlusModule.h
    src/opener/modules/HfsPlusModule.cpp
    src/opener/modules/DmgModule.h
    src/opener/modules/DmgModule.cpp
    src/opener/modules/BtrfsModule.h
    src/opener/modules/BtrfsModule.cpp
    src/opener/modules/RegistryModule.h
    src/opener/modules/RegistryModule.cpp
    src/opener/modules/BootConfigModule.h
    src/opener/modules/BootConfigModule.cpp
    src/opener/modules/CabModule.h
    src/opener/modules/CabModule.cpp
    src/opener/modules/RawDiskModule.h
    src/opener/modules/RawDiskModule.cpp
    src/opener/modules/AndroidSuperModule.h
    src/opener/modules/AndroidSuperModule.cpp
    src/opener/modules/XnbModule.h
    src/opener/modules/XnbModule.cpp
    src/opener/modules/DxbcModule.h
    src/opener/modules/DxbcModule.cpp
    src/opener/modules/NupModule.h
    src/opener/modules/NupModule.cpp
    src/opener/modules/XdbfModule.h
    src/opener/modules/XdbfModule.cpp
    src/opener/modules/FloppyImageModule.h
    src/opener/modules/FloppyImageModule.cpp
    src/opener/modules/AdzModule.h
    src/opener/modules/AdzModule.cpp
    src/opener/modules/ImzModule.h
    src/opener/modules/ImzModule.cpp
    src/opener/modules/FfuModule.h
    src/opener/modules/FfuModule.cpp
    src/opener/modules/ZipModule.h
    src/opener/modules/ZipModule.cpp
    src/opener/modules/TarModule.h
    src/opener/modules/TarModule.cpp
    src/opener/modules/WpiModule.h
    src/opener/modules/WpiModule.cpp
    src/opener/modules/AsarModule.h
    src/opener/modules/AsarModule.cpp
    src/opener/modules/Ps3PupModule.h
    src/opener/modules/Ps3PupModule.cpp
    src/opener/modules/WadModule.h
    src/opener/modules/WadModule.cpp
    src/opener/modules/MtfModule.h
    src/opener/modules/MtfModule.cpp
    src/opener/modules/MdfMdsModule.h
    src/opener/modules/MdfMdsModule.cpp
    src/opener/modules/PlayReadyXapModule.h
    src/opener/modules/PlayReadyXapModule.cpp
    src/opener/modules/EAppxModule.h
    src/opener/modules/EAppxModule.cpp
)
set_source_files_properties(
    src/opener/compression/third_party/lzma/Aes.c
    src/opener/compression/third_party/lzma/Sha256.c
    PROPERTIES COMPILE_DEFINITIONS Z7_NO_CPU_FEATURES
)
set_source_files_properties(
    src/opener/compression/third_party/packmp3/packmp3.cpp
    PROPERTIES COMPILE_DEFINITIONS BUILD_LIB
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    enable_language(ASM)
    target_sources(PeareOpener PRIVATE
        src/opener/compression/third_party/zstd/decompress/huf_decompress_amd64.S
    )
    set_source_files_properties(
        src/opener/compression/third_party/wimlib/src/lzms_common.c
        PROPERTIES COMPILE_DEFINITIONS PEARE_WIMLIB_DISABLE_CPU_FEATURES
    )
endif()
target_compile_definitions(PeareOpener PRIVATE PEARE_OPENER_BUILD)
target_include_directories(PeareOpener
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/zstd
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/wimlib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/wavpack/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/wavpack/src
)
target_link_libraries(PeareOpener PRIVATE
    ${PEARE_QT_CORE} ${PEARE_QT_CORE5COMPAT})
set_target_properties(PeareOpener PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME PeareOpener
)

add_library(PeareDecoder SHARED
    src/decoder/font/AfmFont.h
    src/decoder/font/AfmFont.cpp
    src/decoder/font/PfmFont.h
    src/decoder/font/PfmFont.cpp
    src/decoder/font/AmigaFont.h
    src/decoder/font/AmigaFont.cpp
    src/decoder/font/AmigaAnimFont.h
    src/decoder/font/AmigaAnimFont.cpp
    src/decoder/font/SfnFont.h
    src/decoder/font/SfnFont.cpp
    src/decoder/font/ShpFont.h
    src/decoder/font/ShpFont.cpp
    src/decoder/font/MacFondFont.h
    src/decoder/font/MacFondFont.cpp
    src/decoder/font/SnfFont.h
    src/decoder/font/SnfFont.cpp
    src/decoder/font/SvgFont.h
    src/decoder/font/SvgFont.cpp
    src/decoder/font/BitmapFont.h
    src/decoder/font/BitmapFont.cpp
    src/decoder/font/SunNewsFont.h
    src/decoder/font/SunNewsFont.cpp
    src/decoder/font/DwfFont.h
    src/decoder/font/DwfFont.cpp
    src/decoder/font/MacBitmapSfnt.cpp
    src/decoder/font/SfntFont.h
    src/decoder/font/SfntFont.cpp
    src/decoder/font/EotFont.h
    src/decoder/font/EotFont.cpp
    src/decoder/font/WindowsFnt.h
    src/decoder/font/WindowsFnt.cpp
    src/decoder/font/WinGdiSfntRenderer.h
    src/decoder/font/WinGdiSfntRenderer.cpp
    src/decoder/font/Type1Font.h
    src/decoder/font/Type1Font.cpp
    src/decoder/font/Type3Font.h
    src/decoder/font/Type3Font.cpp
    src/decoder/api/peare/peare.h
    src/opener/api/peare/peare_types.h
    src/decoder/api/peare/peare_decoder.h
    src/decoder/api/Api.cpp
    src/decoder/api/PeareResourceBridge.h
    src/decoder/Optional.h
    src/decoder/DecoderTypes.h
    src/decoder/DecoderRoute.h
    src/decoder/DecoderRoute.cpp
    src/decoder/ResourceDecoder.h
    src/decoder/ResourceDecoder.cpp
    src/decoder/resources/ResourceResolver.h
    src/decoder/resources/ModuleResources.h
    src/decoder/resources/ModuleResources.cpp
    src/decoder/resources/AndroidBinaryXml.h
    src/decoder/resources/AndroidBinaryXml.cpp
    src/decoder/resources/RawDetect.h
    src/decoder/resources/RawDetect.cpp
    src/decoder/resources/WAD/WadImageDecoder.h
    src/decoder/resources/WAD/WadImageDecoder.cpp
    src/decoder/resources/PCX/PcxDecoder.h
    src/decoder/resources/PCX/PcxDecoder.cpp
    src/decoder/resources/GIF/GifRecovery.h
    src/decoder/resources/GIF/GifRecovery.cpp
    src/decoder/ResourceExport.h
    src/decoder/ResourceExport.cpp
    src/decoder/resources/XBE_LOGO_RLE/XBE_LOGO_RLE.h
    src/decoder/resources/XBE_LOGO_RLE/XBE_LOGO_RLE.cpp
    src/decoder/resources/FMIM/FMIM.h
    src/decoder/resources/FMIM/FMIM.cpp
    src/decoder/resources/WSZ/WszDecoder.h
    src/decoder/resources/WSZ/WszDecoder.cpp
    src/decoder/resources/RT_ICON/Win12MonochromeResource.h
    src/decoder/resources/RT_ICON/Win12MonochromeResource.cpp
    src/decoder/resources/RT_ICON/RT_ICON.h
    src/decoder/resources/RT_ICON/RT_ICON.cpp
    src/decoder/resources/RT_GROUP_ICON/RT_GROUP_ICON.h
    src/decoder/resources/RT_GROUP_ICON/RT_GROUP_ICON.cpp
    src/decoder/resources/RT_CURSOR/RT_CURSOR.h
    src/decoder/resources/RT_CURSOR/RT_CURSOR.cpp
    src/decoder/resources/RT_GROUP_CURSOR/RT_GROUP_CURSOR.h
    src/decoder/resources/RT_GROUP_CURSOR/RT_GROUP_CURSOR.cpp
    src/decoder/resources/RT_BITMAP/RT_BITMAP.h
    src/decoder/resources/RT_BITMAP/RT_BITMAP.cpp
    src/decoder/resources/SIEMENS_MWIMAGE/SiemensMwImage.h
    src/decoder/resources/SIEMENS_MWIMAGE/SiemensMwImage.cpp
    src/decoder/resources/SIEMENS_ADONIS/SiemensAdonis.h
    src/decoder/resources/SIEMENS_ADONIS/SiemensAdonis.cpp
    src/decoder/resources/RT_POINTER/RT_POINTER.h
    src/decoder/resources/RT_POINTER/RT_POINTER.cpp
    src/decoder/resources/RT_STRING/RT_STRING.h
    src/decoder/resources/RT_STRING/RT_STRING.cpp
    src/decoder/resources/RT_DLGINCLUDE/RT_DLGINCLUDE.h
    src/decoder/resources/RT_DLGINCLUDE/RT_DLGINCLUDE.cpp
    src/decoder/resources/RT_MESSAGE/RT_MESSAGE.h
    src/decoder/resources/RT_MESSAGE/RT_MESSAGE.cpp
    src/decoder/resources/RT_VERSION/RT_VERSION.h
    src/decoder/resources/RT_VERSION/RT_VERSION.cpp
    src/decoder/resources/RT_ACCELERATOR/RT_ACCELERATOR.h
    src/decoder/resources/RT_ACCELERATOR/RT_ACCELERATOR.cpp
    src/decoder/resources/RT_ACCELTABLE/RT_ACCELTABLE.h
    src/decoder/resources/RT_ACCELTABLE/RT_ACCELTABLE.cpp
    src/decoder/resources/RT_FONT/RT_FONT.h
    src/decoder/resources/RT_FONT/RT_FONT.cpp
    src/decoder/resources/RT_FONT/OS2_RT_FONT.h
    src/decoder/resources/RT_FONT/OS2_RT_FONT.cpp
    src/decoder/resources/RT_FONTDIR/RT_FONTDIR.h
    src/decoder/resources/RT_FONTDIR/RT_FONTDIR.cpp
    src/decoder/resources/RT_DISPLAYINFO/RT_DISPLAYINFO.h
    src/decoder/resources/RT_DISPLAYINFO/RT_DISPLAYINFO.cpp
    src/decoder/resources/RT_HELPTABLE/RT_HELPTABLE.h
    src/decoder/resources/RT_HELPTABLE/RT_HELPTABLE.cpp
    src/decoder/resources/RT_HELPSUBTABLE/RT_HELPSUBTABLE.h
    src/decoder/resources/RT_HELPSUBTABLE/RT_HELPSUBTABLE.cpp
    src/decoder/resources/RT_NAMETABLE/RT_NAMETABLE.h
    src/decoder/resources/RT_NAMETABLE/RT_NAMETABLE.cpp
    src/decoder/resources/RT_MENU/RT_MENU.h
    src/decoder/resources/RT_MENU/RT_MENU.cpp
    src/decoder/resources/RT_MENU/OS2_RT_MENU.h
    src/decoder/resources/RT_MENU/OS2_RT_MENU.cpp
    src/decoder/resources/RT_DIALOG/RT_DIALOG.h
    src/decoder/resources/RT_DIALOG/RT_DIALOG.cpp
    src/decoder/resources/RT_DIALOG/OS2_RT_DIALOG.h
    src/decoder/resources/RT_DIALOG/OS2_RT_DIALOG.cpp
)
target_compile_definitions(PeareDecoder PRIVATE PEARE_DECODER_BUILD)
target_include_directories(PeareDecoder
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/api>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
)
target_link_libraries(PeareDecoder
    PRIVATE ${PEARE_QT_CORE} ${PEARE_QT_GUI} ${PEARE_QT_CORE5COMPAT}
            PeareWoff2Decoder PeareMtxDecoder
)
if(WIN32)
    target_link_libraries(PeareDecoder PRIVATE gdi32)
endif()
set_target_properties(PeareDecoder PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME PeareDecoder
)

add_library(PeareWriter SHARED
    src/common/compression/UnixCompress.h
    src/common/compression/UnixCompress.cpp
    src/common/compression/SevenZipAesDecoder.h
    src/common/compression/SevenZipAesDecoder.cpp
    src/common/compression/SevenZipBcj2Decoder.h
    src/common/compression/SevenZipBcj2Decoder.cpp
    src/writer/api/peare/peare_writer.h
    src/writer/api/Api.cpp
    src/writer/formats/WinZipJpegEncoder.h
    src/writer/formats/WinZipJpegEncoder.cpp
    src/opener/compression/third_party/winzipjpeg/ArithmeticDecoder.c
    src/opener/compression/third_party/winzipjpeg/ArithmeticDecoder.h
    src/opener/compression/third_party/winzipjpeg/JPEG.c
    src/opener/compression/third_party/winzipjpeg/JPEG.h
    src/writer/formats/DiskImageWriter.h
    src/writer/formats/DiskImageWriter.cpp
    src/writer/formats/IsoImageWriter.h
    src/writer/formats/IsoImageWriter.cpp
    src/writer/formats/RarArchiveWriter.h
    src/writer/formats/RarArchiveWriter.cpp
    src/writer/formats/SevenZipArchiveWriter.h
    src/writer/formats/SevenZipArchiveWriter.cpp
    src/writer/formats/WimImageWriter.h
    src/writer/formats/WimImageWriter.cpp
    src/opener/fs/XpressDecoder.cpp
    src/opener/compression/peare/LzxDecoder.cpp
    src/common/rar/Rar5Crypto.h
    src/common/rar/Rar5Crypto.cpp
    src/opener/compression/third_party/lzma/Aes.h
    src/opener/compression/third_party/lzma/Aes.c
    src/common/rar/Rar4Crypto.h
    src/common/rar/Rar4Crypto.cpp
    src/common/rar/Rar4LzDecoder.h
    src/common/rar/Rar4LzDecoder.cpp
    src/common/rar/Rar4Vm.h
    src/common/rar/Rar4Vm.cpp
    src/common/rar/Rar4PpmdDecoder.h
    src/common/rar/Rar4PpmdDecoder.cpp
    src/opener/compression/third_party/lzma/Ppmd7.h
    src/opener/compression/third_party/lzma/Ppmd7.c
    src/opener/compression/third_party/lzma/Ppmd8Dec.c
    src/opener/compression/third_party/lzma/Ppmd7Dec.c
    src/opener/compression/third_party/lzma/Bcj2.c
    src/writer/formats/ExtVolumeUpdate.h
    src/writer/formats/ExtVolumeUpdate.cpp
    src/writer/formats/NtfsVolumeUpdate.h
    src/writer/formats/NtfsVolumeUpdate.cpp
    src/writer/formats/VhdxImageWriter.h
    src/writer/formats/VhdxImageWriter.cpp
    src/writer/formats/VhdImageWriter.h
    src/writer/formats/VhdImageWriter.cpp
    src/writer/formats/VmdkImageWriter.h
    src/writer/formats/VmdkImageWriter.cpp
    src/writer/formats/VmdkStreamOptimizedWriter.h
    src/writer/formats/VmdkStreamOptimizedWriter.cpp
    src/writer/formats/VmdkImageAccess.h
    src/writer/formats/VmdkSplitImageWriter.h
    src/writer/formats/VmdkSplitImageWriter.cpp
    src/writer/third_party/fatfs/LICENSE.txt
    src/writer/third_party/fatfs/diskio.h
    src/writer/third_party/fatfs/ff.c
    src/writer/third_party/fatfs/ff.h
    src/writer/third_party/fatfs/ffconf.h
    src/writer/third_party/fatfs/ffunicode.c
    src/writer/third_party/miniz/LICENSE
    src/writer/third_party/miniz/miniz_common.h
    src/writer/third_party/miniz/miniz.c
    src/writer/third_party/miniz/miniz_tinfl.c
    src/writer/third_party/miniz/miniz.h
    src/writer/third_party/miniz/miniz_export.h
    src/writer/third_party/miniz/miniz_tdef.c
    src/writer/third_party/miniz/miniz_tdef.h
    src/writer/third_party/miniz/miniz_tinfl.h
    src/writer/third_party/miniz/miniz_zip.h
    src/writer/third_party/bzip2/LICENSE
    src/writer/third_party/bzip2/blocksort.c
    src/writer/third_party/bzip2/bzlib.c
    src/writer/third_party/bzip2/bzlib.h
    src/writer/third_party/bzip2/bzlib_private.h
    src/writer/third_party/bzip2/compress.c
    src/writer/third_party/bzip2/crctable.c
    src/writer/third_party/bzip2/decompress.c
    src/writer/third_party/bzip2/huffman.c
    src/writer/third_party/bzip2/randtable.c
    src/writer/third_party/lzma/7zTypes.h
    src/writer/third_party/lzma/7zCrc.c
    src/writer/third_party/lzma/7zCrc.h
    src/writer/third_party/lzma/7zCrcOpt.c
    src/writer/third_party/lzma/Alloc.c
    src/writer/third_party/lzma/Alloc.h
    src/writer/third_party/lzma/7zWindows.h
    src/writer/third_party/lzma/Bra.c
    src/writer/third_party/lzma/Bra86.c
    src/writer/third_party/lzma/Bra.h
    src/writer/third_party/lzma/Compiler.h
    src/writer/third_party/lzma/CpuArch.c
    src/writer/third_party/lzma/CpuArch.h
    src/writer/third_party/lzma/Delta.c
    src/writer/third_party/lzma/Delta.h
    src/writer/third_party/lzma/LICENSE.txt
    src/writer/third_party/lzma/LzFind.c
    src/writer/third_party/lzma/LzFind.h
    src/writer/third_party/lzma/LzHash.h
    src/writer/third_party/lzma/LzmaEnc.c
    src/writer/third_party/lzma/LzmaEnc.h
    src/writer/third_party/lzma/Lzma2Enc.c
    src/writer/third_party/lzma/Lzma2Enc.h
    src/writer/third_party/lzma/Lzma2Dec.c
    src/writer/third_party/lzma/Lzma2Dec.h
    src/writer/third_party/lzma/LzmaDec.c
    src/writer/third_party/lzma/LzmaDec.h
    src/writer/third_party/lzma/Precomp.h
    src/writer/third_party/lzma/RotateDefs.h
    src/writer/third_party/lzma/Sha256.h
    src/writer/third_party/lzma/Sha256.c
    src/writer/third_party/lzma/Sha256Opt.c
    src/writer/third_party/lzma/Xz.h
    src/writer/third_party/lzma/Xz.c
    src/writer/third_party/lzma/XzCrc64.c
    src/writer/third_party/lzma/XzCrc64.h
    src/writer/third_party/lzma/XzCrc64Opt.c
    src/writer/third_party/lzma/XzEnc.c
    src/writer/third_party/lzma/XzEnc.h
    src/writer/third_party/lzma/XzDec.c
    src/writer/third_party/lzma/Ppmd8.h
    src/writer/third_party/lzma/Ppmd.h
    src/writer/third_party/lzma/Ppmd.h
    src/opener/compression/third_party/lzma/Ppmd7.h
    src/opener/compression/third_party/lzma/Ppmd7.c
    src/writer/third_party/lzma/Ppmd8.c
    src/writer/third_party/lzma/Ppmd8Enc.c
    src/writer/third_party/zstd/zstd.h
    src/writer/third_party/zstd/zstd_errors.h
    src/writer/third_party/zstd/LICENSE
    src/writer/third_party/zstd/common/debug.c
    src/writer/third_party/zstd/common/entropy_common.c
    src/writer/third_party/zstd/common/error_private.c
    src/writer/third_party/zstd/common/fse_decompress.c
    src/writer/third_party/zstd/common/pool.c
    src/writer/third_party/zstd/common/threading.c
    src/writer/third_party/zstd/common/xxhash.c
    src/writer/third_party/zstd/common/zstd_common.c
    src/opener/compression/third_party/zstd/decompress/huf_decompress.c
    src/opener/compression/third_party/zstd/decompress/zstd_decompress.c
    src/opener/compression/third_party/zstd/decompress/zstd_decompress_block.c
    src/opener/compression/third_party/zstd/decompress/zstd_ddict.c
    src/writer/third_party/zstd/compress/fse_compress.c
    src/writer/third_party/zstd/compress/hist.c
    src/writer/third_party/zstd/compress/huf_compress.c
    src/writer/third_party/zstd/compress/zstd_compress.c
    src/writer/third_party/zstd/compress/zstd_compress_literals.c
    src/writer/third_party/zstd/compress/zstd_compress_sequences.c
    src/writer/third_party/zstd/compress/zstd_compress_superblock.c
    src/writer/third_party/zstd/compress/zstd_double_fast.c
    src/writer/third_party/zstd/compress/zstd_fast.c
    src/writer/third_party/zstd/compress/zstd_lazy.c
    src/writer/third_party/zstd/compress/zstd_ldm.c
    src/writer/third_party/zstd/compress/zstd_opt.c
    src/writer/third_party/zstd/compress/zstd_preSplit.c
    src/writer/third_party/zstd/compress/zstdmt_compress.c
    src/opener/compression/third_party/packmp3/packmp3.cpp
    src/opener/compression/third_party/packmp3/bitops.cpp
    src/opener/compression/third_party/packmp3/aricoder.cpp
    src/opener/compression/third_party/packmp3/huffmp3.cpp
    src/opener/compression/third_party/packmp3/packmp3lib.h
    src/opener/compression/third_party/packmp3/bitops.h
    src/opener/compression/third_party/packmp3/aricoder.h
    src/opener/compression/third_party/packmp3/huffmp3.h
    src/opener/compression/third_party/packmp3/huffmp3tbl.h
    src/opener/compression/third_party/packmp3/pmp3bitlen.h
    src/opener/compression/third_party/packmp3/pmp3tbl.h
    src/opener/compression/third_party/packmp3/LICENSE
    src/opener/compression/third_party/wavpack/include/wavpack.h
    src/opener/compression/third_party/wavpack/src/common_utils.c
    src/opener/compression/third_party/wavpack/src/decorr_utils.c
    src/opener/compression/third_party/wavpack/src/entropy_utils.c
    src/opener/compression/third_party/wavpack/src/extra1.c
    src/opener/compression/third_party/wavpack/src/extra2.c
    src/opener/compression/third_party/wavpack/src/open_utils.c
    src/opener/compression/third_party/wavpack/src/open_filename.c
    src/opener/compression/third_party/wavpack/src/open_legacy.c
    src/opener/compression/third_party/wavpack/src/open_raw.c
    src/opener/compression/third_party/wavpack/src/pack.c
    src/opener/compression/third_party/wavpack/src/pack_dns.c
    src/opener/compression/third_party/wavpack/src/pack_floats.c
    src/opener/compression/third_party/wavpack/src/pack_utils.c
    src/opener/compression/third_party/wavpack/src/read_words.c
    src/opener/compression/third_party/wavpack/src/tags.c
    src/opener/compression/third_party/wavpack/src/tag_utils.c
    src/opener/compression/third_party/wavpack/src/unpack.c
    src/opener/compression/third_party/wavpack/src/unpack_floats.c
    src/opener/compression/third_party/wavpack/src/unpack_seek.c
    src/opener/compression/third_party/wavpack/src/unpack_utils.c
    src/opener/compression/third_party/wavpack/src/write_words.c
    src/opener/compression/third_party/wavpack/src/decorr_tables.h
    src/opener/compression/third_party/wavpack/src/unpack3.h
    src/opener/compression/third_party/wavpack/src/wavpack_local.h
    src/opener/compression/third_party/wavpack/src/wavpack_version.h
    src/opener/compression/third_party/wavpack/LICENSE.txt
    src/common/compression/DeflateDecoder.cpp
    src/common/compression/DeflateDecoder.h
    src/opener/compression/LzmaDecoder.cpp
    src/opener/compression/LzmaDecoder.h
    src/opener/compression/peare/Bzip2Decoder.cpp
    src/opener/compression/peare/Bzip2Decoder.h
)
target_compile_definitions(PeareWriter PRIVATE PEARE_WRITER_BUILD ZSTD_DISABLE_ASM)
if(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set_source_files_properties(
        src/writer/third_party/lzma/Sha256Opt.c
        PROPERTIES COMPILE_OPTIONS "-march=armv8-a+sha2"
    )
endif()
set_source_files_properties(src/writer/third_party/miniz/miniz.c PROPERTIES
    COMPILE_DEFINITIONS "MINIZ_NO_TIME")
set_source_files_properties(
    src/writer/third_party/lzma/LzFind.c
    src/writer/third_party/lzma/LzmaEnc.c
    src/writer/third_party/lzma/Lzma2Enc.c
    src/writer/third_party/lzma/XzEnc.c
    src/writer/third_party/lzma/XzDec.c
    PROPERTIES COMPILE_DEFINITIONS "Z7_ST")
target_include_directories(PeareWriter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/writer/api>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/writer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/modules
        ${CMAKE_CURRENT_SOURCE_DIR}/src/writer/third_party/bzip2
        ${CMAKE_CURRENT_SOURCE_DIR}/src/writer/third_party/zstd
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/packmp3
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/wavpack/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/compression/third_party/wavpack/src
)
target_link_libraries(PeareWriter PRIVATE ${PEARE_QT_CORE})
set_target_properties(PeareWriter PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME PeareWriter
)

add_library(PeareExtFs STATIC
    src/writer/third_party/lwext4/include/ext4.h
    src/writer/third_party/lwext4/include/ext4_balloc.h
    src/writer/third_party/lwext4/include/ext4_bcache.h
    src/writer/third_party/lwext4/include/ext4_bitmap.h
    src/writer/third_party/lwext4/include/ext4_block_group.h
    src/writer/third_party/lwext4/include/ext4_blockdev.h
    src/writer/third_party/lwext4/include/ext4_config.h
    src/writer/third_party/lwext4/include/ext4_crc32.h
    src/writer/third_party/lwext4/include/ext4_casefold.h
    src/writer/third_party/lwext4/include/ext4_debug.h
    src/writer/third_party/lwext4/include/ext4_dir.h
    src/writer/third_party/lwext4/include/ext4_dir_idx.h
    src/writer/third_party/lwext4/include/ext4_errno.h
    src/writer/third_party/lwext4/include/ext4_extent.h
    src/writer/third_party/lwext4/include/ext4_fs.h
    src/writer/third_party/lwext4/include/ext4_hash.h
    src/writer/third_party/lwext4/include/ext4_ialloc.h
    src/writer/third_party/lwext4/include/ext4_inode.h
    src/writer/third_party/lwext4/include/ext4_journal.h
    src/writer/third_party/lwext4/include/ext4_mbr.h
    src/writer/third_party/lwext4/include/ext4_misc.h
    src/writer/third_party/lwext4/include/ext4_mkfs.h
    src/writer/third_party/lwext4/include/ext4_oflags.h
    src/writer/third_party/lwext4/include/ext4_super.h
    src/writer/third_party/lwext4/include/ext4_trans.h
    src/writer/third_party/lwext4/include/ext4_types.h
    src/writer/third_party/lwext4/include/ext4_xattr.h
    src/writer/third_party/lwext4/src/ext4.c
    src/writer/third_party/lwext4/src/ext4_balloc.c
    src/writer/third_party/lwext4/src/ext4_bcache.c
    src/writer/third_party/lwext4/src/ext4_bitmap.c
    src/writer/third_party/lwext4/src/ext4_block_group.c
    src/writer/third_party/lwext4/src/ext4_blockdev.c
    src/writer/third_party/lwext4/src/ext4_crc32.c
    src/writer/third_party/lwext4/src/ext4_casefold.c
    src/writer/third_party/lwext4/src/ext4_casefold_data.h
    src/writer/third_party/lwext4/src/ext4_debug.c
    src/writer/third_party/lwext4/src/ext4_dir.c
    src/writer/third_party/lwext4/src/ext4_dir_idx.c
    src/writer/third_party/lwext4/src/ext4_extent.c
    src/writer/third_party/lwext4/src/ext4_fs.c
    src/writer/third_party/lwext4/src/ext4_hash.c
    src/writer/third_party/lwext4/src/ext4_ialloc.c
    src/writer/third_party/lwext4/src/ext4_inode.c
    src/writer/third_party/lwext4/src/ext4_journal.c
    src/writer/third_party/lwext4/src/ext4_mkfs.c
    src/writer/third_party/lwext4/src/ext4_super.c
    src/writer/third_party/lwext4/src/ext4_trans.c
)
target_compile_definitions(PeareExtFs PRIVATE
    CONFIG_USE_DEFAULT_CFG=1
    CONFIG_EXT_FEATURE_SET_LVL=4
    CONFIG_EXTENTS_ENABLE=1
    CONFIG_XATTR_ENABLE=0
    CONFIG_JOURNALING_ENABLE=1
    CONFIG_DEBUG_PRINTF=0
    CONFIG_DEBUG_ASSERT=0
)
target_include_directories(PeareExtFs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/writer/third_party/lwext4/include
)
set_target_properties(PeareExtFs PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(PeareWriter PRIVATE PeareExtFs)
set_source_files_properties(src/writer/formats/ExtVolumeUpdate.cpp PROPERTIES
    COMPILE_DEFINITIONS "CONFIG_USE_DEFAULT_CFG=1;CONFIG_EXT_FEATURE_SET_LVL=4;CONFIG_EXTENTS_ENABLE=1;CONFIG_XATTR_ENABLE=0;CONFIG_JOURNALING_ENABLE=1;CONFIG_DEBUG_PRINTF=0;CONFIG_DEBUG_ASSERT=0"
)

include(src/writer/third_party/ntfs-3g/PeareNtfs3g.cmake)

set(PEARE_APP_SOURCES
    src/app/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/FileAssociationSettingsDialog.cpp
    src/app/FileAssociationSettingsDialog.h
    src/app/PeareApi.h
    src/app/PeareApi.cpp
    src/app/IconFromExt.h
    src/app/IconFromExt.cpp
    src/app/SettingsIcons.h
    src/app/SettingsIcons.cpp
    resources/peare.qrc
)

if(ANDROID AND QT_VERSION_MAJOR GREATER_EQUAL 6)
    # qt_add_executable creates the Android shared-library application target
    # and the deployment metadata used by androiddeployqt. Qt Widgets remains
    # the GUI technology; no QML conversion is required.
    qt_add_executable(Peare MANUAL_FINALIZATION ${PEARE_APP_SOURCES})
else()
    add_executable(Peare ${PEARE_APP_SOURCES})
endif()

if(WIN32)
    target_sources(Peare PRIVATE resources/peare.rc)
endif()

if(APPLE AND NOT ANDROID)
    set_target_properties(Peare PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.peare.app"
        MACOSX_BUNDLE_BUNDLE_NAME "Peare"
    )
    target_link_libraries(Peare PRIVATE "-framework CoreServices")
endif()

target_include_directories(Peare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/opener/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/writer/api
)
target_link_libraries(Peare PRIVATE PeareOpener PeareDecoder PeareWriter ${PEARE_QT_WIDGETS})
if(WIN32)
    target_link_libraries(Peare PRIVATE shell32 shlwapi advapi32)
endif()

add_custom_command(TARGET Peare POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/filetypes"
        "$<TARGET_FILE_DIR:Peare>/filetypes"
)

if(ANDROID AND QT_VERSION_MAJOR GREATER_EQUAL 6)
    set_property(TARGET Peare PROPERTY QT_ANDROID_PACKAGE_NAME "org.peare.app")
    qt_finalize_executable(Peare)
endif()

install(TARGETS PeareOpener PeareDecoder PeareWriter
    EXPORT PeareTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY resources/filetypes DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    src/decoder/api/peare/peare.h
    src/opener/api/peare/peare_types.h
    src/opener/api/peare/peare_opener.h
    src/decoder/api/peare/peare_decoder.h
    src/writer/api/peare/peare_writer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/peare
)

configure_package_config_file(
    cmake/PeareConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT PeareTargets
    FILE PeareTargets.cmake
    NAMESPACE Peare::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/PeareConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Peare
)
