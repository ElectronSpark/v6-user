# AddXv6Program.cmake — declare an xv6-native user program.
#
# Each program lives at ${CMAKE_SOURCE_DIR}/programs/<name>/<name>.c.
# The built binary is named `_<name>` to match xv6 convention (the
# leading underscore distinguishes the unstripped ELF from the user-
# visible command name `<name>` used inside the shell).
#
# Usage:
#   xv6_user_program(<name>)
#       Builds programs/<name>/<name>.c into target _<name>, links it
#       against userlib, and installs it as bin/_<name>.
#
# Variables consumed (set by the parent CMakeLists.txt):
#   USER_LINKER_SCRIPT   — full path to user.ld
#   USER_LIB_DIR         — path to user/lib (for headers)

function(xv6_user_program NAME)
    set(_target  _${NAME})
    set(_source  ${CMAKE_SOURCE_DIR}/programs/${NAME}/${NAME}.c)

    add_executable(${_target} ${_source})
    target_link_libraries(${_target} PRIVATE userlib)
    target_include_directories(${_target} PRIVATE ${USER_LIB_DIR})

    # The xv6 user link recipe: -nostdlib, page-aligned, custom linker
    # script. Mirrored verbatim from xv6-tmp/user/CMakeLists.txt.
    target_link_options(${_target} PRIVATE
        -nostdlib
        -Wl,-z,max-page-size=4096
        -Wl,-e,start
        -Wl,-u,start
        -T ${USER_LINKER_SCRIPT})

    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME ${_target}
        SUFFIX "")

    install(TARGETS ${_target} RUNTIME DESTINATION bin)
endfunction()
