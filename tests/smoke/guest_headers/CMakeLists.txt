cmake_minimum_required(VERSION 3.20)
project(guest_headers CXX)
set(CMAKE_CXX_STANDARD 20)
add_library(guest_headers STATIC guest_headers.cpp guest_middleware.cpp)
