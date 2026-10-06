# cannet_canopen_dictionary(<target> FILE <file.od> NAME <identifier>
#                           [NAMESPACE <namespace>] [HEADER <name.hpp>])
#
# Compiles a device's dictionary into <target> from an OD file
# (canopen/od_file.hpp): generates a header declaring
#
#   inline constexpr auto <identifier> = cannet::canopen::dictionary{...};
#
# in <namespace>, which <target> includes as "<name.hpp>", by default
# "<identifier>.hpp". The header is made again whenever the OD file changes,
# by `canopen od header`, built for the purpose even where cannet's tools are
# not; the dictionary is checked when compiled, as any other. <target> must
# link cannet::canopen.
function(cannet_canopen_dictionary target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "FILE;NAME;NAMESPACE;HEADER" "")
    if(NOT arg_FILE OR NOT arg_NAME)
        message(FATAL_ERROR "cannet_canopen_dictionary: FILE and NAME are required")
    endif()
    if(NOT arg_HEADER)
        set(arg_HEADER "${arg_NAME}.hpp")
    endif()

    get_filename_component(file "${arg_FILE}" ABSOLUTE)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/cannet_od/${target}")
    set(header "${dir}/${arg_HEADER}")
    set(namespace)
    if(arg_NAMESPACE)
        set(namespace --namespace "${arg_NAMESPACE}")
    endif()

    add_custom_command(
        OUTPUT "${header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${dir}"
        COMMAND canopen-cli od header "${file}" --name "${arg_NAME}"
                ${namespace} --output "${header}"
        DEPENDS "${file}" canopen-cli
        COMMENT "Generating ${arg_HEADER} from ${arg_FILE}"
        VERBATIM
    )
    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
