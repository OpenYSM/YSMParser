# CMakeList.txt : CMake project for YSMParser, include source and define
# project specific logic here.
#

# Add source to this project's executable.
add_executable (YSMParser  "parsers/YSMParser.hpp" "main.cpp" "parsers/v3/PlayerTextureCollector.hpp" "parsers/v3/YSGPHeaderParser.hpp" "parsers/YSMParser.hpp" "parsers/YSMParser.cpp" "parsers/v3/YSMParserV3.hpp" "parsers/YSMParserV2.hpp"  "parsers/v3/YSMParserV3.cpp" "parsers/exceptions/ParserException.hpp"  "parsers/YSMParserV2.cpp" "parsers/YSMParserV1.cpp" "parsers/YSMParserV1.hpp" "parsers/BufferReader.hpp" "algorithms/CryptoAlgorithms.hpp" "algorithms/CryptoAlgorithms.cpp" "algorithms/YsmZstd.hpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET YSMParser PROPERTY CXX_STANDARD 20)
endif()

if (YSM_TARGET_WASM)
  set_target_properties(YSMParser PROPERTIES
    OUTPUT_NAME "YSMParser"
  )
else()
  set_target_properties(YSMParser PROPERTIES
    OUTPUT_NAME "YSMParser"
    VERSION ${PROJECT_VERSION}
  )
endif()

target_compile_definitions(YSMParser PRIVATE
  YSM_PARSER_VERSION="${PROJECT_VERSION}"
)

if (MSVC)
  target_compile_definitions(YSMParser PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if (YSM_TARGET_WASM)
  target_compile_options(YSMParser PRIVATE
    "-fexceptions"
  )

  if (YSM_WASM_SIMD)
    target_compile_options(YSMParser PRIVATE
      "-msimd128"
    )
  endif()

  target_link_options(YSMParser PRIVATE
    "-fexceptions"
    "-sFORCE_FILESYSTEM=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXIT_RUNTIME=0"
  )

  if (YSM_WASM_SIMD)
    target_link_options(YSMParser PRIVATE
      "-msimd128"
    )
  endif()

  if (YSM_WASM_ENV STREQUAL "node")
    target_link_options(YSMParser PRIVATE
      "-sENVIRONMENT=node"
      "-sNODERAWFS=1"
    )
  elseif (YSM_WASM_ENV STREQUAL "web")
    target_link_options(YSMParser PRIVATE
      "-sENVIRONMENT=web"
      "-sMODULARIZE=1"
      "-sEXPORT_NAME=YSMParserModule"
      "-sEXPORTED_RUNTIME_METHODS=['FS','callMain']"
    )
  endif()
endif()


target_link_libraries(YSMParser PRIVATE cityhash)
target_link_libraries(YSMParser PRIVATE libzstd_static)
target_link_libraries(YSMParser PRIVATE xchacha20)
target_link_libraries(YSMParser PRIVATE AES)
target_link_libraries(YSMParser PRIVATE md5)
target_link_libraries(YSMParser PRIVATE base64)
target_link_libraries(YSMParser PRIVATE zlibstatic)
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/zstd/lib")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/cityhash/src")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/xchacha20/src")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/AES/src")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/md5")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/base64-cpp")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/zlib")
target_include_directories(YSMParser PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/json")

target_link_libraries(YSMParser PRIVATE fpng)

install(TARGETS YSMParser
  RUNTIME DESTINATION .
  BUNDLE DESTINATION .
)

if (YSM_TARGET_WASM)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/YSMParser.wasm" DESTINATION . OPTIONAL)
endif()

# --- JNI Shared Library ---
if(YSM_TARGET_JNI)
  add_library(YSMParserJNI SHARED
    "parsers/YSMParserJNI.cpp"
    "parsers/YSMParser.hpp"
    "parsers/YSMParser.cpp"
    "parsers/v3/YSMParserV3.hpp"
    "parsers/v3/YSMParserV3.cpp"
    "parsers/YSMParserV2.hpp"
    "parsers/YSMParserV2.cpp"
    "parsers/YSMParserV1.cpp"
    "parsers/YSMParserV1.hpp"
    "parsers/BufferReader.hpp"
    "parsers/exceptions/ParserException.hpp"
    "algorithms/CryptoAlgorithms.hpp"
    "algorithms/CryptoAlgorithms.cpp"
    "algorithms/NativeAlgorithmsJNI.cpp"
    "parsers/v3/PlayerTextureCollector.hpp"
    "parsers/v3/YSGPHeaderParser.hpp"
    "platform/PlatformCompat.hpp"
   "algorithms/YsmZstd.hpp")

  if(CMAKE_VERSION VERSION_GREATER 3.12)
    set_property(TARGET YSMParserJNI PROPERTY CXX_STANDARD 20)
  endif()

  target_compile_definitions(YSMParserJNI PRIVATE
    YSM_PARSER_VERSION="${PROJECT_VERSION}"
  )

  if(MSVC)
    target_compile_definitions(YSMParserJNI PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()

  target_include_directories(YSMParserJNI PRIVATE ${JNI_INCLUDE_DIRS})

  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/zstd/lib")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/cityhash/src")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/xchacha20/src")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/AES/src")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/md5")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/base64-cpp")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/zlib")
  target_include_directories(YSMParserJNI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../external/json")

  target_link_libraries(YSMParserJNI PRIVATE cityhash libzstd_static xchacha20 AES md5 base64 zlibstatic fpng)

  if(WIN32)
    set_target_properties(YSMParserJNI PROPERTIES PREFIX "" SUFFIX ".dll")
  elseif(APPLE)
    set_target_properties(YSMParserJNI PROPERTIES PREFIX "lib" SUFFIX ".dylib")
  else()
    set_target_properties(YSMParserJNI PROPERTIES PREFIX "lib" SUFFIX ".so")
  endif()

  install(TARGETS YSMParserJNI
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
  )
endif()
