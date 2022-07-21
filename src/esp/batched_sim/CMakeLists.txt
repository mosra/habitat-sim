option(MAGNUM_RENDERER "Use Magnum renderer instead of BPS3D" ON)

# Required by Magnum
if(MAGNUM_RENDERER)
  if(NOT BUILD_WITH_CUDA)
    message(FATAL_ERROR "BUILD_WITH_CUDA has to be enabled for the Magnum Batch Renderer")
  endif()
# Required by BPS3D
else()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED 17)
endif()

# BpsImporter plugin
if(MAGNUM_RENDERER)
  find_package(Corrade REQUIRED PluginManager)
  find_package(Magnum REQUIRED GL GlfwApplication SceneTools Trade MeshTools Shaders TextureTools DebugTools WindowlessEglApplication)
  if(MAGNUM_BUILD_STATIC)
    find_package(MagnumPlugins REQUIRED BasisImporter)
    corrade_add_static_plugin(BpsImporter ";" BpsImporter.conf BpsImporter.cpp)
  else()
    corrade_add_plugin(BpsImporter "/" "/" BpsImporter.conf BpsImporter.cpp)
  endif()
  target_link_libraries(BpsImporter PUBLIC Magnum::Trade)
endif()

if(MAGNUM_RENDERER AND NOT MAGNUM_BUILD_STATIC)
  set(BPSIMPORTER_PLUGIN_FILENAME $<TARGET_FILE:BpsImporter>)
endif()

# First replace ${} variables, then $<> generator expressions
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/configure.h.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/configure.h.in)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/configure.h
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/configure.h.in)

# The batched_sim library
set(batched_sim_SRCS
  BatchedSimAssert.h
  BatchedSimulator.cpp
  BatchedSimulator.h
  ColumnGrid.cpp
  ColumnGrid.h
  CollisionBroadphaseGrid.cpp
  CollisionBroadphaseGrid.h
  ColumnGridBuilder.cpp
  ColumnGridBuilder.h
  EpisodeGenerator.cpp
  EpisodeGenerator.h
  EpisodeSet.cpp
  EpisodeSet.h
  GlmUtils.cpp # TODO the GLM-less stuff should be split away from here
  GlmUtils.h
  PlacementHelper.cpp
  PlacementHelper.h
  ProfilingScope.cpp
  ProfilingScope.h
  SerializeCollection.cpp
  SerializeCollection.h
)
if(MAGNUM_RENDERER)
  list(APPEND batched_sim_SRCS
    MagnumRenderer.cpp
    MagnumRenderer.h
    MagnumRendererStandalone.cpp
    MagnumRendererStandalone.h
  )
else()
  list(APPEND batched_sim_SRCS
    BpsSceneMapping.cpp
    BpsSceneMapping.h
  )
endif()
add_library(batched_sim STATIC ${batched_sim_SRCS})
target_link_libraries(
  batched_sim
  PUBLIC core sim)
target_include_directories(batched_sim PRIVATE ${CMAKE_BINARY_DIR})
if(MAGNUM_RENDERER)
  target_include_directories(batched_sim PRIVATE
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
    ${CMAKE_CURRENT_LIST_DIR}/../gfx/cuda_helpers
  )
  target_link_libraries(batched_sim PUBLIC ${CUDART_LIBRARY})
  target_link_libraries(batched_sim PRIVATE
    Magnum::GL
    Magnum::Magnum
    Magnum::Shaders
    Magnum::Trade
    Magnum::MeshTools
    Magnum::WindowlessEglApplication)
  if(MAGNUM_BUILD_STATIC)
    target_link_libraries(batched_sim PRIVATE
      MagnumPlugins::BasisImporter
      BpsImporter)
  else()
    add_dependencies(batched_sim BpsImporter)
  endif()
else()
  target_link_libraries(
    batched_sim
    PUBLIC
    io
    bps3D
    bps3D_core
    bps3D_vulkan
    stb
  )
endif()

# Converter and standalone demo for Magnum renderer
if(MAGNUM_RENDERER)
  add_executable(MagnumRendererConverter
    MagnumRendererConverter.cpp)
  target_link_libraries(MagnumRendererConverter PRIVATE
    Magnum::SceneTools
    Magnum::Trade
    Magnum::MeshTools
    Magnum::TextureTools)
  if(MAGNUM_BUILD_STATIC)
    target_link_libraries(MagnumRendererConverter PRIVATE
      Magnum::AnySceneImporter
      MagnumPlugins::AssimpImporter
      MagnumPlugins::StbImageImporter
      MagnumPlugins::KtxImageConverter
      MagnumPlugins::GltfSceneConverter
      MagnumPlugins::GltfImporter)
  endif()

  add_executable(MagnumRendererDemo
    MagnumRenderer.cpp
    MagnumRendererDemo.cpp)
  target_link_libraries(MagnumRendererDemo PRIVATE
    Magnum::Application
    Magnum::DebugTools
    Magnum::GL
    Magnum::Magnum
    Magnum::Shaders
    Magnum::Trade
    Magnum::MeshTools)
  target_include_directories(MagnumRendererDemo PRIVATE
    ${PROJECT_BINARY_DIR}
  )
  if(MAGNUM_BUILD_STATIC)
    target_link_libraries(MagnumRendererDemo PRIVATE
      Magnum::AnySceneImporter
      MagnumPlugins::BasisImporter
      MagnumPlugins::GltfImporter
      MagnumPlugins::KtxImporter
      BpsImporter)
  else()
    add_dependencies(MagnumRendererDemo BpsImporter)
  endif()
endif()
