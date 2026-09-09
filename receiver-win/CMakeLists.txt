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
project(controller_receiver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull the ViGEmClient SDK at configure time (needs internet + git).
# If the tag ever 404s, check https://github.com/nefarius/ViGEmClient/tags
# or clone it next to this file and replace FetchContent with
# add_subdirectory(ViGEmClient).
include(FetchContent)
FetchContent_Declare(
    ViGEmClient
    GIT_REPOSITORY https://github.com/nefarius/ViGEmClient.git
    GIT_TAG        v1.21.222.0
)
FetchContent_MakeAvailable(ViGEmClient)

add_executable(receiver main.cpp)
target_include_directories(receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
    ${vigemclient_SOURCE_DIR}/include)
target_link_libraries(receiver PRIVATE ViGEmClient ws2_32 setupapi shell32)
