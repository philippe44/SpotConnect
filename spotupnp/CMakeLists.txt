cmake_minimum_required(VERSION 3.0)

if(WIN32)
	set(HOST Win32)
	set(PLATFORM x86)
	set(PROJECT "spotupnp")
    set(LIBRARY_SUFFIX ".lib")
else()
	if(NOT HOST OR NOT PLATFORM)
		if(CMAKE_C_COMPILER)
			set(C_COMPILER ${CMAKE_C_COMPILER})
			message(STATUS "using CMAKE_C_COMPILER ${CMAKE_C_COMPILER}")
		else()
			execute_process(COMMAND bash "-c" "readlink -f `which cc`" OUTPUT_VARIABLE C_COMPILER)
			cmake_path(GET C_COMPILER FILENAME C_COMPILER)
			message(STATUS "guessing compiler ${C_COMPILER}")
		endif()

		string(REGEX MATCHALL "([^-]+)" CC_SPLIT ${C_COMPILER})

		list(GET CC_SPLIT 0 PLATFORM)
		list(LENGTH CC_SPLIT C_TUPLE_SIZE)

		if(C_TUPLE_SIZE EQUAL 5)
			list(GET CC_SPLIT 1 HOST)
		else()
			list(GET CC_SPLIT 2 HOST)
		endif()
	endif()
    set(LIBRARY_SUFFIX ".a")
	set(PROJECT "spotupnp-${HOST}-${PLATFORM}")
endif()

if(NOT HOST OR NOT PLATFORM)
	message(STATUS "cannot define HOST (${HOST}) and/or PLATFORM ${PLATFORM}")
	return()
endif()

set(BASE ${CMAKE_CURRENT_SOURCE_DIR}/..)
project(${PROJECT})

message(STATUS "Base directory is ${BASE}")
message(STATUS "Using CC ${CMAKE_C_COMPILER} and ${CMAKE_CXX_COMPILER}")
message(STATUS "Building for ${HOST}-${PLATFORM} with cmake's definition ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# Configurable options
option(USE_ALSA "Enable ALSA" OFF)
option(USE_PORTAUDIO "Enable PortAudio" OFF)
set(CMAKE_BUILD_TYPE Debug CACHE STRING "CMake Build Type")

# @TODO Full command line, for the forgetful
# cmake <cmake-root-file-dir> -A Win32 -DBELL_EXTERNAL_MBEDTLS=<mbedtls-build-dir>\cmake
# cmake <cmake-root-file-dir> [-DCMAKE_SYSTEM_NAME=<cmake-os>] [-DCMAKE_SYSTEM_PROCESSOR=<cmake-cpu>] [-DCMAKE_C_COMPILER=<cc>] [-DCMAKE_CXX_COMPILER=<cxx>] [-DHOST=<os> [-DPLATFORM=<cpu>] -DBELL_EXTERNAL_MBEDTLS=<mbedtls-build-dir>/cmake

set(BELL_DISABLE_CODECS ON)
set(BELL_VORBIS_FLOAT ON)
set(BELL_DISABLE_SINKS ON)
set(BELL_DISABLE_AVAHI ON)
set(BELL_DISABLE_MQTT ON)

# set(BELL_DISABLE_FMT ON)
# set(BELL_DISABLE_REGEX ON)
# set(BELL_ONLY_CJSON ON)

# CMake options
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

if(MSVC)
	add_compile_definitions(NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE UPNP_USE_MSVCPP)
	add_definitions(/wd4068 /wd4244 /wd4018 /wd4101 /wd4102 /wd4142 /wd4996 /wd4090)
else()
	add_compile_options(-O2 -fdata-sections -ffunction-sections)
endif()

# Main target sources
file(GLOB SOURCES src/*.cpp src/*.c ${BASE}/common/crosstools/src/*.c )
list(REMOVE_ITEM SOURCES ${BASE}/common/crosstools/src/cross_ssl.c)
list(APPEND EXTRA_INCLUDES src ${BASE}/common)
add_executable(${PROJECT} ${SOURCES})

# Other includes
list(APPEND EXTRA_INCLUDES ${BASE}/common/crosstools/src)

# libpupnp
list(APPEND EXTRA_INCLUDES ${BASE}/common/libpupnp/targets/${HOST}/${PLATFORM}/include/upnp)
list(APPEND EXTRA_INCLUDES ${BASE}/common/libpupnp/targets/${HOST}/${PLATFORM}/include/ixml)
list(APPEND EXTRA_INCLUDES ${BASE}/common/libpupnp/targets/${HOST}/${PLATFORM}/include/addons)
list(APPEND EXTRA_LIBS ${BASE}/common/libpupnp/targets/${HOST}/${PLATFORM}/libpupnp${LIBRARY_SUFFIX})

# codecs with cmake
find_package(libcodecs REQUIRED CONFIG PATHS ${BASE}/common/libcodecs)
get_target_property(_INFO libcodecs::codecs INTERFACE_INCLUDE_DIRECTORIES)
list(APPEND EXTRA_INCLUDES "${_INFO}")
get_target_property(_INFO libcodecs::codecs IMPORTED_LOCATION_RELEASE)
set(BELL_EXTERNAL_VORBIS "${_INFO}" CACHE STRING "")
# or do manually
# target_include_directories(${PROJECT} PRIVATE ${BASE}/common/libcodecs/targets/include/flac ${BASE}/common/libcodecs/targets/include/shine)
# target_include_directories(${PROJECT} PRIVATE ${BASE}/common/libcodecs/targets/include/ogg ${BASE}/common/libcodecs/targets/include/vorbis)
# set(BELL_EXTERNAL_VORBIS ${BASE}/common/libcodecs/targets/${HOST}/${PLATFORM}/libcodecs})

# pthreads
if(MSVC)
    list(APPEND EXTRA_INCLUDES ${BASE}/common/libpthreads4w/targets/${HOST}/${PLATFORM}/include)
    list(APPEND EXTRA_LIBS ${BASE}/common/libpthreads4w/targets/${HOST}/${PLATFORM}/libpthreadVC3${LIBRARY_SUFFIX})
endif()

# Import cspot library
# codecs are from include above
add_subdirectory(${BASE}/common/cspot/cspot ${CMAKE_CURRENT_BINARY_DIR}/cspot)
target_include_directories(cspot PRIVATE ${BASE}/common/libcodecs/targets/include/vorbis)
target_include_directories(cspot PRIVATE ${BASE}/common/libcodecs/targets/include/ogg)

# Configure the target
if(NOT MSVC)
	if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "SunOS")
        add_executable(${PROJECT}-static ${SOURCES})
        target_link_options(${PROJECT}-static PUBLIC -static)
        target_include_directories(${PROJECT}-static PRIVATE "." ${EXTRA_INCLUDES})
        target_compile_definitions(${PROJECT}-static PRIVATE -DFLAC__NO_DLL -DUPNP_STATIC_LIB -D_GNU_SOURCE)
        target_link_libraries(${PROJECT}-static PUBLIC cspot ${EXTRA_LIBS})
	endif()
	if(CMAKE_SYSTEM_NAME STREQUAL SunOS)
		set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE} -lssp")
	endif()
    
endif()

target_include_directories(${PROJECT} PRIVATE "." ${EXTRA_INCLUDES})
target_compile_definitions(${PROJECT} PRIVATE -DFLAC__NO_DLL -DUPNP_STATIC_LIB -D_GNU_SOURCE)
target_link_libraries(${PROJECT} PUBLIC cspot ${EXTRA_LIBS})