cmake_minimum_required(VERSION 3.20)
project(gridscape-backend-cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# cpp-httplib — header-only HTTP server/client
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
)
FetchContent_MakeAvailable(httplib)

# nlohmann/json — header-only JSON
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# libcurl — required for outbound HTTPS calls to the LLM API
find_package(CURL REQUIRED)

add_executable(gridscape main.cpp)
target_link_libraries(gridscape PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CURL::libcurl
)
