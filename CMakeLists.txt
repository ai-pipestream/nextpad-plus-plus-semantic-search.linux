cmake_minimum_required(VERSION 3.20)
project(SemanticSearchLinux VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pango)
find_package(Threads REQUIRED)

# ONNX Runtime is dlopen'd. The plugin and the tests do not link it.
option(NPP_WITH_ONNXRUNTIME "Compile the ONNX Runtime loader" ON)
set(ONNXRUNTIME_ROOT "" CACHE PATH
    "ONNX Runtime prefix baked into the loader (lib/ or lib64/). Empty uses the environment at run time.")
set(NPP_SEMANTIC_TEST_MODEL_DIR "" CACHE PATH
    "Directory with MiniLM ONNX files for the embedding checks")

set(NPP_HAVE_ONNXRUNTIME 0)
if(NPP_WITH_ONNXRUNTIME)
    set(NPP_HAVE_ONNXRUNTIME 1)
endif()

set(NPP_HEADERS "${CMAKE_SOURCE_DIR}/third_party/npp")

add_library(npp_semantic STATIC
    src/engine/JsonMini.cpp
    src/engine/WordPieceTokenizer.cpp
    src/engine/SentenceSplitter.cpp
    src/engine/CpuSimilarityEngine.cpp
    src/engine/OnnxEmbeddingProvider.cpp
    src/engine/ModelZoo.cpp
    src/engine/BackgroundWorker.cpp
    src/engine/CancellableSpawn.cpp
    src/engine/DocumentTrack.cpp
    src/engine/EmbedFallback.cpp
    src/engine/OrtRuntime.cpp
)
target_include_directories(npp_semantic PUBLIC
    "${CMAKE_SOURCE_DIR}/src/engine"
    "${CMAKE_SOURCE_DIR}/src/engine/ort"
)
target_compile_definitions(npp_semantic PUBLIC
    NPP_HAVE_ONNXRUNTIME=${NPP_HAVE_ONNXRUNTIME}
    "NPP_ONNXRUNTIME_ROOT=\"${ONNXRUNTIME_ROOT}\""
)
target_link_libraries(npp_semantic PUBLIC
    PkgConfig::GLIB2 PkgConfig::PANGO Threads::Threads ${CMAKE_DL_LIBS})

add_library(SemanticSearch SHARED
    src/PluginEntry.cpp
    src/EditorBridge.cpp
    src/SemanticController.cpp
)
target_include_directories(SemanticSearch PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${NPP_HEADERS}"
)
target_link_libraries(SemanticSearch PRIVATE npp_semantic PkgConfig::GTK4 ${CMAKE_DL_LIBS})
target_link_options(SemanticSearch PRIVATE "-Wl,--allow-shlib-undefined")
set_target_properties(SemanticSearch PROPERTIES
    PREFIX ""
    OUTPUT_NAME "SemanticSearch"
)

set(PLUGIN_INSTALL_DIR "$ENV{HOME}/.local/share/nextpad++/plugins/SemanticSearch"
    CACHE PATH "Plugin directory (folder name must match the .so)")
install(TARGETS SemanticSearch DESTINATION "${PLUGIN_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/resources/semantic/models.json"
        DESTINATION "${PLUGIN_INSTALL_DIR}/resources/semantic")
install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION "${PLUGIN_INSTALL_DIR}")

enable_testing()

add_executable(npp_semantic_tests
    tests/semantic/test_semantic.cpp
    src/indicator_alloc.c
)
target_include_directories(npp_semantic_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(npp_semantic_tests PRIVATE npp_semantic)
target_compile_definitions(npp_semantic_tests PRIVATE
    SEMANTIC_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/semantic"
    SEMANTIC_MANIFEST="${CMAKE_SOURCE_DIR}/resources/semantic/models.json"
    SEMANTIC_MODEL_DIR="${NPP_SEMANTIC_TEST_MODEL_DIR}"
)
add_test(NAME semantic COMMAND npp_semantic_tests)

add_executable(npp_semantic_lifecycle tests/semantic/test_lifecycle.cpp)
target_link_libraries(npp_semantic_lifecycle PRIVATE npp_semantic)
add_test(NAME semantic-lifecycle COMMAND npp_semantic_lifecycle)
set_tests_properties(semantic-lifecycle PROPERTIES TIMEOUT 20)

add_executable(npp_ort_probe tests/semantic/ort_probe.cpp)
target_link_libraries(npp_ort_probe PRIVATE npp_semantic)
add_test(NAME semantic-ort-probe COMMAND npp_ort_probe)
set_tests_properties(semantic-ort-probe PROPERTIES TIMEOUT 20)

add_executable(plugin_loader tests/plugin_loader.cpp)
target_include_directories(plugin_loader PRIVATE "${NPP_HEADERS}")
target_link_libraries(plugin_loader PRIVATE PkgConfig::GTK4 ${CMAKE_DL_LIBS})
add_test(NAME plugin_loader COMMAND plugin_loader $<TARGET_FILE:SemanticSearch>)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME semantic-runtime-detection
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/semantic/test_runtime_detection.py")
    add_test(NAME semantic-ort-absent
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/semantic/test_ort_load.py"
                $<TARGET_FILE:npp_ort_probe>
                $<TARGET_FILE:npp_semantic_tests>
                $<TARGET_FILE:npp_semantic_lifecycle>
                $<TARGET_FILE:SemanticSearch>)
    set_tests_properties(semantic-ort-absent PROPERTIES TIMEOUT 30)
endif()
