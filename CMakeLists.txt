project(iocp_driver)

#字符集unicode
Add_Definitions(-DUNICODE -D_UNICODE)


# Find includes in the build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)



set(src_files
    iocp_driver.cpp
)

set(inc_files
    iocp_driver.h
)

add_library(${PROJECT_NAME}
    ${src_files}    
    ${inc_files}
    
)

target_include_directories(${PROJECT_NAME}
    PUBLIC .
)


target_link_libraries(${PROJECT_NAME} 
)

add_executable(echo_server
    test/server.cpp
)

target_link_libraries(echo_server
    ws2_32
    ${PROJECT_NAME}
)
set_target_properties(echo_server PROPERTIES
  LINK_FLAGS /SUBSYSTEM:CONSOLE
)

add_executable(echo_client
    test/client.cpp
)

target_link_libraries(echo_client
    ws2_32
    ${PROJECT_NAME}
)

set_target_properties(echo_client PROPERTIES
  LINK_FLAGS /SUBSYSTEM:CONSOLE
)