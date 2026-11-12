if(DEFINED PEARE_SOURCE_ROOT)
    set(repository_root "${PEARE_SOURCE_ROOT}")
else()
    set(repository_root "${CMAKE_SOURCE_DIR}")
endif()

execute_process(
    COMMAND git -C "${repository_root}" ls-files --cached
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE tracked_paths
    ERROR_VARIABLE git_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT git_result EQUAL 0)
    message(FATAL_ERROR "Cannot validate repository root: ${git_error}")
endif()

string(REPLACE "\n" ";" tracked_paths "${tracked_paths}")
set(approved_root_entries ".git")
foreach(tracked_path IN LISTS tracked_paths)
    if(tracked_path STREQUAL "")
        continue()
    endif()
    string(REGEX MATCH "^[^/]+" root_entry "${tracked_path}")
    if(NOT root_entry STREQUAL "")
        list(APPEND approved_root_entries "${root_entry}")
    endif()
endforeach()
list(REMOVE_DUPLICATES approved_root_entries)

file(GLOB root_entries LIST_DIRECTORIES TRUE
    "${repository_root}/*"
    "${repository_root}/.*"
)
set(unapproved_root_entries)
foreach(root_path IN LISTS root_entries)
    get_filename_component(root_entry "${root_path}" NAME)
    if(root_entry STREQUAL "." OR root_entry STREQUAL "..")
        continue()
    endif()
    list(FIND approved_root_entries "${root_entry}" approved_index)
    if(NOT approved_index EQUAL -1)
        continue()
    endif()

    if(IS_DIRECTORY "${root_path}" AND
       (root_entry MATCHES "^build(-[A-Za-z0-9._]+)?$" OR
        root_entry STREQUAL "tmp" OR
        root_entry STREQUAL "dist"))
        continue()
    endif()

    list(APPEND unapproved_root_entries "${root_entry}")
endforeach()

if(unapproved_root_entries)
    list(JOIN unapproved_root_entries ", " unapproved_text)
    message(FATAL_ERROR
        "Unapproved entries found in the project root: ${unapproved_text}. "
        "Move source samples into their documented fixture directories and "
        "generated files into an approved output directory before building.")
endif()
