# controller-streamer - stream a game controller over the LAN to a Windows PC
# Copyright (C) 2026 knmn2000
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
cmake_minimum_required(VERSION 3.16)
project(controller_sender CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- SDL2 ---------------------------------------------------------------------
# macOS and Linux get SDL2 from a package manager, which ships a CMake config,
# so find_package just works. Windows has no such convention.
#
# FETCH_SDL2=ON downloads the official prebuilt VC package for you. It is opt-in
# rather than automatic on purpose: a build file that silently reaches out to the
# network is hard to trust and hard to debug, and file(DOWNLOAD) can fail while
# leaving an empty file behind - so the download is explicitly status-checked.
option(FETCH_SDL2 "Download the prebuilt SDL2 VC package (Windows convenience)" OFF)

find_package(SDL2 QUIET)

if(NOT SDL2_FOUND AND WIN32 AND FETCH_SDL2)
    set(SDLVER 2.30.9)
    set(SDLROOT "${CMAKE_BINARY_DIR}/SDL2-${SDLVER}")
    if(NOT EXISTS "${SDLROOT}/cmake/sdl2-config.cmake")
        set(SDLZIP "${CMAKE_BINARY_DIR}/SDL2-devel-${SDLVER}-VC.zip")
        message(STATUS "Downloading SDL2 ${SDLVER} (prebuilt VC package)")
        file(DOWNLOAD
            "https://github.com/libsdl-org/SDL/releases/download/release-${SDLVER}/SDL2-devel-${SDLVER}-VC.zip"
            "${SDLZIP}"
            EXPECTED_HASH SHA256=8c91d91e5bcb997d062ec2b553c53832ebf95654d4aa35e8c02a954d4ce752ae
            STATUS SDLDL
            SHOW_PROGRESS)
        list(GET SDLDL 0 SDLDLCODE)
        if(NOT SDLDLCODE EQUAL 0)
            list(GET SDLDL 1 SDLDLMSG)
            message(FATAL_ERROR
                "SDL2 download failed (${SDLDLMSG}).\n"
                "Fetch it yourself and point CMake at it instead:\n"
                "  -DSDL2_DIR=<dir containing sdl2-config.cmake>")
        endif()
        # cmake -E tar, not file(ARCHIVE_EXTRACT): the latter extracted nothing
        # here and reported no error, which is worse than failing.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xf "${SDLZIP}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            RESULT_VARIABLE SDLUNZIP)
        if(NOT SDLUNZIP EQUAL 0)
            message(FATAL_ERROR "could not extract ${SDLZIP} (code ${SDLUNZIP})")
        endif()
    endif()
    set(SDL2_DIR "${SDLROOT}/cmake" CACHE PATH "" FORCE)
    find_package(SDL2 REQUIRED)
endif()

if(NOT SDL2_FOUND)
    message(FATAL_ERROR
        "SDL2 not found. Pick one:\n"
        "  macOS    brew install sdl2\n"
        "  Linux    sudo apt install libsdl2-dev\n"
        "  Windows  cmake -B build -DFETCH_SDL2=ON        (downloads it)\n"
        "           or vcpkg install sdl2:x64-windows\n"
        "           or -DSDL2_DIR=<dir containing sdl2-config.cmake>")
endif()

add_executable(sender main.cpp)
target_include_directories(sender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

if(TARGET SDL2::SDL2)
    target_link_libraries(sender PRIVATE SDL2::SDL2)
else() # older SDL2 configs export variables instead of targets
    target_include_directories(sender PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(sender PRIVATE ${SDL2_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(sender PRIVATE ws2_32)
endif()

if(MSVC)
    # main.cpp has no BOM but contains non-ASCII characters in comments. Without
    # /utf-8 MSVC decodes the file in the legacy codepage, which is the same
    # class of bug that once broke setup-pc.ps1 (see its header comment).
    target_compile_options(sender PRIVATE /utf-8)
endif()
