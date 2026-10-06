if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
    message(FATAL_ERROR "PROBE must name an existing executable")
endif()

function(check_cli label expected_exit expected_stdout expected_stderr)
    execute_process(
        COMMAND "${PROBE}" ${ARGN}
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
        TIMEOUT 3
    )
    if(NOT "${actual_exit}" STREQUAL "${expected_exit}"
       OR NOT "${actual_stdout}" STREQUAL "${expected_stdout}"
       OR NOT "${actual_stderr}" STREQUAL "${expected_stderr}")
        message(FATAL_ERROR "${label}: exit=${actual_exit}, stdout=[${actual_stdout}], stderr=[${actual_stderr}]")
    endif()
endfunction()

set(usage "usage: siftwing_build_probe <single-byte> <bytes>\n")
check_cli(mixed 0 "3\n" "" a banana)
check_cli(absent 0 "0\n" "" z banana)
check_cli(all 0 "4\n" "" a aaaa)
check_cli(missing 2 "" "${usage}")
check_cli(too_many 2 "" "${usage}" a banana extra)
check_cli(multi_byte_needle 2 "" "${usage}" ab banana)

# Quoted empty arguments must reach argv; expanding ARGN drops empty list items.
foreach(empty_case IN ITEMS bytes needle)
    if(empty_case STREQUAL "bytes")
        execute_process(COMMAND "${PROBE}" a "" RESULT_VARIABLE code
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
        set(expected_code 0)
        set(expected_output "0\n")
        set(expected_error "")
    else()
        execute_process(COMMAND "${PROBE}" "" banana RESULT_VARIABLE code
            OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
        set(expected_code 2)
        set(expected_output "")
        set(expected_error "${usage}")
    endif()
    if(NOT "${code}" STREQUAL "${expected_code}"
       OR NOT "${output}" STREQUAL "${expected_output}"
       OR NOT "${error}" STREQUAL "${expected_error}")
        message(FATAL_ERROR "empty ${empty_case}: exit=${code}, stdout=[${output}], stderr=[${error}]")
    endif()
endforeach()
