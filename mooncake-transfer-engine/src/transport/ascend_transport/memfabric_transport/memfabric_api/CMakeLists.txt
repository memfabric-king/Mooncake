file(GLOB MEM_FABRIC_API_SOURCE "*.cpp")

set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

string(REPLACE "-O3" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "-O3" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

add_library(mem_fabric_api SHARED ${MEM_FABRIC_API_SOURCE})
set_target_properties(mem_fabric_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(mem_fabric_api PRIVATE glog gflags)
