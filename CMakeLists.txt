cmake_minimum_required(VERSION 3.21)

macro(set_from_environment VARIABLE)
	if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif()
endmacro()

set_from_environment(VCPKG_ROOT)
set_from_environment(Fallout4Path)

if(DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else()
	message(WARNING "VCPKG_ROOT is not set. Continuing without vcpkg.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

project(
	BetterWeaponScrappingF4SE
	VERSION 1.0.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
foreach(_cfg RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/cmake")

if(NOT TARGET CommonLibF4)
	add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/CommonLibF4" CommonLibF4)
endif()

find_package(spdlog REQUIRED CONFIG)
find_package(imgui CONFIG REQUIRED)

set(SOURCES
	src/PCH.cpp
	src/main.cpp
	src/Settings.cpp
	src/BWSMenu.cpp
	src/GamepadInput.cpp
	src/ScrapManager.cpp
	src/ScrapOverlay.cpp
	src/ScrapModManager.cpp
	src/ExamineMenuBridge.cpp
	DetourXS/detourxs.cpp
)

set(HEADERS
	src/PCH.h
	src/Settings.h
	src/BWSMenu.h
	src/GamepadInput.h
	src/BenchScrapTypes.h
	src/ScrapManager.h
	src/ScrapOverlay.h
	src/ScrapModManager.h
	src/ExamineMenuBridge.h
	src/BWS_RendererData.h
	DetourXS/detourxs.h
)

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE
	_UNICODE
	UNICODE
	NOMINMAX
	_CRT_SECURE_NO_WARNINGS
	_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)

target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
	"${CMAKE_CURRENT_SOURCE_DIR}/../F4SE-Menu-Framework-3/resources"
)

# detourxs.h pulls in LDE64x64 via `#pragma comment(lib, "DetourXS/LDE64x64")`;
# make that relative path resolvable from the project root.
target_link_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE
	CommonLibF4::CommonLibF4
	spdlog::spdlog
	imgui::imgui
	d3d11
	dxgi
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

# Vendored third-party code: keep our F4SE precompiled header out of it.
set_source_files_properties(DetourXS/detourxs.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if(MSVC)
	target_compile_options(${PROJECT_NAME} PRIVATE
		/sdl /utf-8 /Zi /permissive- /Zc:preprocessor /EHsc
		/W4 /WX-
		/wd4099 /wd4100 /wd4189 /wd4244 /wd4302 /wd4311 /wd5054
	)
	target_link_options(${PROJECT_NAME} PRIVATE
		"$<$<CONFIG:DEBUG>:/INCREMENTAL>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif()

option(COPY_BUILD "Copy the build output to the Fallout 4 directory." OFF)
if(COPY_BUILD AND DEFINED Fallout4Path)
	add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
	)
endif()
