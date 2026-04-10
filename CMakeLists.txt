cmake_minimum_required(VERSION 3.24)

project(obs-dfn3-noise-suppress VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DFN3_STRICT_WARNINGS "Enable strict warnings" ON)
set(OBS_ROOT_DIR "" CACHE PATH "Path to OBS Studio SDK/install root (used when libobsConfig.cmake is unavailable)")

find_package(libobs QUIET)

set(_obs_target "")
if(TARGET OBS::libobs)
	set(_obs_target OBS::libobs)
elseif(TARGET libobs::libobs)
	set(_obs_target libobs::libobs)
elseif(TARGET libobs)
	set(_obs_target libobs)
else()
	set(_obs_include_hints "")
	set(_obs_config_hints "")
	set(_obs_library_hints "")

	if(OBS_ROOT_DIR)
		list(APPEND _obs_include_hints
			"${OBS_ROOT_DIR}/include"
			"${OBS_ROOT_DIR}/libobs"
			"${OBS_ROOT_DIR}/obs-studio/libobs"
			"${OBS_ROOT_DIR}/build/libobs"
		)
		list(APPEND _obs_config_hints
			"${OBS_ROOT_DIR}/build/config"
			"${OBS_ROOT_DIR}/config"
			"${OBS_ROOT_DIR}/obs-studio/build/config"
		)
		list(APPEND _obs_library_hints
			"${OBS_ROOT_DIR}/lib"
			"${OBS_ROOT_DIR}/lib64"
			"${OBS_ROOT_DIR}/bin/64bit"
			"${OBS_ROOT_DIR}/build/libobs"
			"${OBS_ROOT_DIR}/build/libobs/RelWithDebInfo"
			"${OBS_ROOT_DIR}/build/libobs/Release"
			"${OBS_ROOT_DIR}/build/libobs/Debug"
			"${OBS_ROOT_DIR}/build/rundir/RelWithDebInfo/lib"
			"${OBS_ROOT_DIR}/build/rundir/Release/lib"
			"${OBS_ROOT_DIR}/build/rundir/Debug/lib"
			"${OBS_ROOT_DIR}/build/rundir/RelWithDebInfo/bin/64bit"
			"${OBS_ROOT_DIR}/build/rundir/Release/bin/64bit"
			"${OBS_ROOT_DIR}/build/rundir/Debug/bin/64bit"
		)
	endif()

	find_path(OBS_INCLUDE_DIR
		NAMES obs-module.h
		HINTS ${_obs_include_hints}
		PATH_SUFFIXES include libobs
	)

	find_path(OBS_CONFIG_INCLUDE_DIR
		NAMES obsconfig.h
		HINTS ${_obs_config_hints}
		PATH_SUFFIXES config
	)

	find_library(OBS_LIBRARY
		NAMES obs libobs
		HINTS ${_obs_library_hints}
	)

	if(OBS_INCLUDE_DIR AND OBS_LIBRARY)
		set(_obs_interface_include_dirs "${OBS_INCLUDE_DIR}")
		if(OBS_CONFIG_INCLUDE_DIR)
			list(APPEND _obs_interface_include_dirs "${OBS_CONFIG_INCLUDE_DIR}")
		endif()

		add_library(obs_manual::libobs UNKNOWN IMPORTED)
		set_target_properties(obs_manual::libobs PROPERTIES
			IMPORTED_LOCATION "${OBS_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${_obs_interface_include_dirs}"
		)
		set(_obs_target obs_manual::libobs)
	else()
		message(FATAL_ERROR
			"Could not locate libobs. Provide libobsConfig.cmake via CMAKE_PREFIX_PATH or set OBS_ROOT_DIR to an OBS SDK/build root.")
	endif()
endif()

add_library(obs-dfn3-noise-suppress MODULE
	src/plugin-main.cpp
	src/dfn3_filter.cpp
	src/df_runtime.cpp
)

target_include_directories(obs-dfn3-noise-suppress PRIVATE
	src
)

target_link_libraries(obs-dfn3-noise-suppress PRIVATE ${_obs_target})

if(WIN32)
	target_compile_definitions(obs-dfn3-noise-suppress PRIVATE
		WIN32_LEAN_AND_MEAN
		NOMINMAX
		_CRT_SECURE_NO_WARNINGS
	)
endif()

if(DFN3_STRICT_WARNINGS)
	if(MSVC)
		target_compile_options(obs-dfn3-noise-suppress PRIVATE /W4 /permissive-)
	else()
		target_compile_options(obs-dfn3-noise-suppress PRIVATE -Wall -Wextra -Wpedantic)
	endif()
endif()

set_target_properties(obs-dfn3-noise-suppress PROPERTIES
	PREFIX ""
	OUTPUT_NAME "obs-dfn3-noise-suppress"
)

if(WIN32)
	set(OBS_PLUGIN_RUNTIME_DEST "obs-plugins/64bit")
else()
	set(OBS_PLUGIN_RUNTIME_DEST "obs-plugins")
endif()

install(TARGETS obs-dfn3-noise-suppress
	RUNTIME DESTINATION ${OBS_PLUGIN_RUNTIME_DEST}
	LIBRARY DESTINATION ${OBS_PLUGIN_RUNTIME_DEST}
)

install(DIRECTORY data/
	DESTINATION data/obs-plugins/obs-dfn3-noise-suppress
)
