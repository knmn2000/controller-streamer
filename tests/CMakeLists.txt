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
project(controller_streamer_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every tool here builds against the real protocol/protocol.h, never a copy.
set(PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

if(MSVC)
    # Sources contain non-ASCII in comments; without /utf-8 MSVC decodes them
    # in the legacy codepage. Same class of bug as the setup-pc.ps1 header note.
    add_compile_options(/utf-8)
endif()

# --- protocol_test: portable, no hardware, no network -------------------------
# The one test that must pass everywhere. Verifies struct sizes, big-endian wire
# bytes, the INT16_MIN axis edge case, RFC-1982 sequence wraparound and every
# mapping helper.
add_executable(protocol_test protocol_test.cpp)
target_include_directories(protocol_test PRIVATE ${PROTO})
if(WIN32)
    target_link_libraries(protocol_test PRIVATE ws2_32)   # htons/ntohs
endif()
enable_testing()
add_test(NAME protocol COMMAND protocol_test)

# --- POSIX-only ---------------------------------------------------------------
if(NOT WIN32)
    # Plays the receiver's role on localhost so the desktop sender can be
    # exercised end to end without a Windows box.
    add_executable(loopback_receiver loopback_receiver.cpp)
    target_include_directories(loopback_receiver PRIVATE ${PROTO})
endif()

# --- Windows-only diagnostics -------------------------------------------------
if(WIN32)
    # Reads the virtual pads back through XInput: list / capture / watch /
    # rumble. `capture <slot> <secs>` records per-axis extremes, which is how
    # you prove stick orientation and trigger range without trusting a web page.
    add_executable(xinput-probe xinput-probe.cpp)
    target_link_libraries(xinput-probe PRIVATE xinput)

    # Drives the receiver with real protocol v3 packets from the same machine,
    # so the receiver can be tested with no phone, Mac or second PC involved.
    add_executable(fake-sender fake-sender.cpp)
    target_include_directories(fake-sender PRIVATE ${PROTO})
    target_link_libraries(fake-sender PRIVATE ws2_32)

    # Prints the exact VIGEM_ERROR from connect/target_add. The receiver only
    # says "failed", and the code is what separates a missing driver from a
    # client/bus version mismatch from a bus that has not settled yet.
    include(FetchContent)
    FetchContent_Declare(ViGEmClient
        GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
        GIT_TAG        v1.21.222.0)     # same pin as receiver-win
    FetchContent_MakeAvailable(ViGEmClient)
    add_executable(vigem-diag vigem-diag.cpp)
    target_include_directories(vigem-diag PRIVATE ${vigemclient_SOURCE_DIR}/include)
    target_link_libraries(vigem-diag PRIVATE ViGEmClient setupapi)
endif()
