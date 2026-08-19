cmake_minimum_required(VERSION 3.26)

if (NOT DEFINED UTILS_FILE)
    message(FATAL_ERROR "UTILS_FILE is required")
endif ()
include("${UTILS_FILE}")

if (DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(tmp_dir "$ENV{TMPDIR}")
else ()
    set(tmp_dir "/tmp")
endif ()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef test_suffix)
set(test_root "${tmp_dir}/zvec_apply_patch_once_${test_suffix}")
set(target_dir "${test_root}/target")
set(old_patch "${test_root}/old.patch")
set(new_patch "${test_root}/new.patch")
set(mark_file "${target_dir}/.fixture_patched")

function(run_git)
    execute_process(
            COMMAND git ${ARGN}
            WORKING_DIRECTORY "${target_dir}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE stdout
            ERROR_VARIABLE stderr)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed:\n${stdout}${stderr}")
    endif ()
endfunction()

function(assert_file path expected)
    file(READ "${path}" actual)
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR
                "Unexpected contents in ${path}: expected '${expected}', got '${actual}'")
    endif ()
endfunction()

file(MAKE_DIRECTORY "${target_dir}")
run_git(init)
run_git(config user.email apply-patch-once-test@example.com)
run_git(config user.name apply-patch-once-test)
file(WRITE "${target_dir}/kept.txt" "base kept\n")
file(WRITE "${target_dir}/removed.txt" "base removed\n")
run_git(add kept.txt removed.txt)
run_git(commit -m base)

# The old patch changes two files.
file(WRITE "${target_dir}/kept.txt" "old kept\n")
file(WRITE "${target_dir}/removed.txt" "old removed\n")
execute_process(
        COMMAND git diff --binary
        WORKING_DIRECTORY "${target_dir}"
        OUTPUT_FILE "${old_patch}"
        COMMAND_ERROR_IS_FATAL ANY)
run_git(checkout -- kept.txt removed.txt)

# The replacement changes only one of them. A recovery based on the new
# patch's file list would leave the old change in removed.txt behind.
file(WRITE "${target_dir}/kept.txt" "new kept\n")
execute_process(
        COMMAND git diff --binary
        WORKING_DIRECTORY "${target_dir}"
        OUTPUT_FILE "${new_patch}"
        COMMAND_ERROR_IS_FATAL ANY)
run_git(checkout -- kept.txt)

# A v2 state file contains the applied old patch and can reverse every old
# hunk before applying the replacement.
apply_patch_once("fixture" "${target_dir}" "${old_patch}")
apply_patch_once("fixture" "${target_dir}" "${new_patch}")
assert_file("${target_dir}/kept.txt" "new kept\n")
assert_file("${target_dir}/removed.txt" "base removed\n")

# A missing state file is repaired when the current patch is already applied.
file(REMOVE "${mark_file}")
apply_patch_once("fixture" "${target_dir}" "${new_patch}")
if (NOT EXISTS "${mark_file}")
    message(FATAL_ERROR "Missing patch state was not recreated")
endif ()

# A submodule reset leaves the state behind; the current patch is reapplied.
run_git(checkout -- kept.txt removed.txt)
apply_patch_once("fixture" "${target_dir}" "${new_patch}")
assert_file("${target_dir}/kept.txt" "new kept\n")
assert_file("${target_dir}/removed.txt" "base removed\n")

# Migrate a legacy boolean marker by using the explicitly retained old patch.
run_git(checkout -- kept.txt removed.txt)
run_git(apply "${old_patch}")
file(WRITE "${mark_file}" "patched")
apply_patch_once("fixture" "${target_dir}" "${new_patch}"
        LEGACY_PATCHES "${old_patch}")
assert_file("${target_dir}/kept.txt" "new kept\n")
assert_file("${target_dir}/removed.txt" "base removed\n")

file(REMOVE_RECURSE "${test_root}")
message(STATUS "apply_patch_once regression test passed")
