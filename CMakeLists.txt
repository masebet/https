PROJECT(main)
CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
# SET(CMAKE_BUILD_TYPE "Debug") 

SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -Wall -g -ggdb")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -Wall")

set(INC_DIR /usr/local/include)
set(LINK_DIR /usr/local/lib)

include_directories(${INC_DIR})
link_directories(${LINK_DIR})

add_compile_options(-std=c++11)
AUX_SOURCE_DIRECTORY(. DIR_SRCS)
ADD_EXECUTABLE(main.out ${DIR_SRCS})
TARGET_LINK_LIBRARIES(main.out -lpthread -lssl -lcrypto)
