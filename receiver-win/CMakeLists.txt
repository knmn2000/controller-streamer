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
