# Promote a target's INTERFACE_INCLUDE_DIRECTORIES to be treated as SYSTEM
# includes by consumers, suppressing warnings from third-party headers.
function(mark_target_includes_system)
    foreach(_target ${ARGN})
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_aliased ${_target} ALIASED_TARGET)
        if(_aliased)
            set(_target ${_aliased})
        endif()
        get_target_property(_inc ${_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            set_target_properties(${_target} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
    endforeach()
endfunction()

function(_git_patch_command result_var stdout_var stderr_var target_dir
        patch_file reverse check)
    set(command git apply)
    if (reverse)
        list(APPEND command --reverse)
    endif ()
    if (check)
        list(APPEND command --check)
    endif ()
    list(APPEND command ${ARGN} --ignore-space-change --ignore-whitespace
            "${patch_file}")

    execute_process(
            COMMAND ${command}
            WORKING_DIRECTORY "${target_dir}"
            RESULT_VARIABLE command_result
            OUTPUT_VARIABLE command_stdout
            ERROR_VARIABLE command_stderr
    )
    set(${result_var} "${command_result}" PARENT_SCOPE)
    set(${stdout_var} "${command_stdout}" PARENT_SCOPE)
    set(${stderr_var} "${command_stderr}" PARENT_SCOPE)
endfunction()

function(_save_patch_state patch_file state_file)
    # The state file is the exact patch that was applied, not a boolean marker.
    # Keeping the old patch lets a later version restore files which disappear
    # from the new patch before applying the replacement.
    configure_file("${patch_file}" "${state_file}" COPYONLY)
endfunction()

function(apply_patch_once patch_name target_dir patch_file)
    # Keep the old filename so existing worktrees can be migrated in place,
    # but treat it as a state file rather than a boolean marker.
    set(state_file "${target_dir}/.${patch_name}_patched")
    cmake_parse_arguments(PATCH "" "" "LEGACY_PATCHES" ${ARGN})
    set(patch_options ${PATCH_UNPARSED_ARGUMENTS})

    if (NOT EXISTS "${patch_file}")
        message(FATAL_ERROR "Patch file '${patch_file}' not found!")
    endif ()

    foreach (legacy_patch IN LISTS PATCH_LEGACY_PATCHES)
        if (NOT EXISTS "${legacy_patch}")
            message(FATAL_ERROR "Legacy patch file '${legacy_patch}' not found!")
        endif ()
    endforeach ()

    # The working tree is the source of truth. This also repairs a missing or
    # legacy boolean marker when the current patch is already fully applied.
    _git_patch_command(reverse_check_result unused_stdout unused_stderr
            "${target_dir}" "${patch_file}" TRUE TRUE ${patch_options})
    if (reverse_check_result EQUAL 0)
        _save_patch_state("${patch_file}" "${state_file}")
        return()
    endif ()

    # A clean tree (including one reset by `git submodule update`) can accept
    # the current patch directly, regardless of what a stale marker says.
    _git_patch_command(forward_check_result unused_stdout forward_check_stderr
            "${target_dir}" "${patch_file}" FALSE TRUE ${patch_options})
    if (forward_check_result EQUAL 0)
        _git_patch_command(patch_result patch_stdout patch_stderr
                "${target_dir}" "${patch_file}" FALSE FALSE ${patch_options})
        if (NOT patch_result EQUAL 0)
            message(FATAL_ERROR
                    "Failed to apply patch '${patch_name}' to ${target_dir}:\n${patch_stderr}")
        endif ()
        _save_patch_state("${patch_file}" "${state_file}")
        return()
    endif ()

    # The current patch is neither applied nor directly applicable. First try
    # the saved patch, then explicitly supplied pre-v2 patches for migration.
    # Unlike a list derived from the new patch, these contain every old hunk.
    set(previous_patches ${PATCH_LEGACY_PATCHES})
    if (EXISTS "${state_file}")
        list(PREPEND previous_patches "${state_file}")
    endif ()

    foreach (previous_patch IN LISTS previous_patches)
        _git_patch_command(previous_reverse_check unused_stdout unused_stderr
                "${target_dir}" "${previous_patch}" TRUE TRUE ${patch_options})
        if (NOT previous_reverse_check EQUAL 0)
            continue()
        endif ()

        message(STATUS
                "Replacing previously applied patch '${patch_name}' in ${target_dir}.")
        _git_patch_command(previous_reverse_result unused_stdout previous_reverse_stderr
                "${target_dir}" "${previous_patch}" TRUE FALSE ${patch_options})
        if (NOT previous_reverse_result EQUAL 0)
            message(FATAL_ERROR
                    "Failed to reverse the previous patch '${patch_name}':\n${previous_reverse_stderr}")
        endif ()

        _git_patch_command(replacement_check unused_stdout replacement_check_stderr
                "${target_dir}" "${patch_file}" FALSE TRUE ${patch_options})
        if (NOT replacement_check EQUAL 0)
            _git_patch_command(rollback_result unused_stdout rollback_stderr
                    "${target_dir}" "${previous_patch}" FALSE FALSE ${patch_options})
            message(FATAL_ERROR
                    "The previous patch '${patch_name}' was reversed, but its replacement "
                    "cannot be applied:\n${replacement_check_stderr}\n"
                    "Rollback result: ${rollback_result}\n${rollback_stderr}")
        endif ()

        _git_patch_command(patch_result patch_stdout patch_stderr
                "${target_dir}" "${patch_file}" FALSE FALSE ${patch_options})
        if (NOT patch_result EQUAL 0)
            _git_patch_command(rollback_result unused_stdout rollback_stderr
                    "${target_dir}" "${previous_patch}" FALSE FALSE ${patch_options})
            message(FATAL_ERROR
                    "Failed to apply replacement patch '${patch_name}':\n${patch_stderr}\n"
                    "Rollback result: ${rollback_result}\n${rollback_stderr}")
        endif ()

        _save_patch_state("${patch_file}" "${state_file}")
        return()
    endforeach ()

    message(FATAL_ERROR
            "Patch '${patch_name}' is neither applied nor applicable in ${target_dir}. "
            "No recorded previous patch matches the working tree. Refusing to reset "
            "tracked files because they may contain local changes.\n"
            "git apply --check output:\n${forward_check_stderr}")
endfunction()
