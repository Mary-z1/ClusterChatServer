cmake_minimum_required(VERSION 3.10)
project(ChatServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/include
                    ${PROJECT_SOURCE_DIR}/thirdparty)

find_package(OpenSSL REQUIRED)

set(SERVER_SOURCES
    src/server/main.cpp
    src/server/server.cpp
    src/server/service.cpp
    src/server/db.cpp
    src/server/redis.cpp
    src/server/model.cpp
    src/server/logger.cpp)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
add_executable(chat_server ${SERVER_SOURCES})

target_link_libraries(chat_server
    muduo_net muduo_base
    mysqlclient
    OpenSSL::SSL OpenSSL::Crypto
    hiredis
    pthread)