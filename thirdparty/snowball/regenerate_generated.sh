#!/usr/bin/env bash
# Regenerate the vendored Snowball UTF-8 C sources consumed by CMake.
#
# This script must be run on a POSIX environment with GNU Make, a C compiler,
# Perl, and sed. Normal Windows/Linux CMake builds do not run Snowball codegen;
# they compile the vendored files under thirdparty/snowball/generated.
#
# Usage:
#   thirdparty/snowball/regenerate_generated.sh [snowball_source_dir]
#
# The default snowball_source_dir is the snowball-* directory next to this
# script.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ $# -gt 1 ]]; then
  echo "usage: $0 [snowball_source_dir]" >&2
  exit 1
fi

if [[ $# -eq 1 ]]; then
  snowball_src_dir="$(cd "$1" && pwd)"
else
  snowball_src_dir="$(find "${script_dir}" -maxdepth 1 -mindepth 1 -type d -name 'snowball-*' | sort | tail -n 1)"
fi

if [[ -z "${snowball_src_dir}" || ! -f "${snowball_src_dir}/GNUmakefile" ]]; then
  echo "error: cannot find Snowball source directory with GNUmakefile" >&2
  exit 1
fi

generated_dir="${script_dir}/generated"

echo "Snowball source directory: ${snowball_src_dir}"
echo "Generated directory:       ${generated_dir}"

algorithms="$(awk '
  /^[ \t]*#/ { next }
  /^[ \t]*$/ { next }
  $2 ~ /UTF_8/ { print $1 }
' "${snowball_src_dir}/libstemmer/modules.txt")"

if [[ -z "${algorithms}" ]]; then
  echo "error: no UTF_8 algorithms found in modules.txt" >&2
  exit 1
fi

make_targets=(libstemmer/libstemmer_utf8.c libstemmer/modules_utf8.h)
while IFS= read -r algorithm; do
  make_targets+=("src_c/stem_UTF_8_${algorithm}.c")
done <<< "${algorithms}"

make -C "${snowball_src_dir}" "${make_targets[@]}"

rm -rf "${generated_dir}"
mkdir -p \
  "${generated_dir}/include" \
  "${generated_dir}/libstemmer" \
  "${generated_dir}/runtime" \
  "${generated_dir}/src_c"

cp "${snowball_src_dir}/include/libstemmer.h" "${generated_dir}/include/"
cp "${snowball_src_dir}/libstemmer/libstemmer_utf8.c" "${generated_dir}/libstemmer/"
cp "${snowball_src_dir}/libstemmer/modules_utf8.h" "${generated_dir}/libstemmer/"
cp "${snowball_src_dir}/runtime/api.c" "${generated_dir}/runtime/"
cp "${snowball_src_dir}/runtime/api.h" "${generated_dir}/runtime/"
cp "${snowball_src_dir}/runtime/snowball_runtime.h" "${generated_dir}/runtime/"
cp "${snowball_src_dir}/runtime/utilities.c" "${generated_dir}/runtime/"
cp "${snowball_src_dir}"/src_c/stem_UTF_8_*.c "${generated_dir}/src_c/"
cp "${snowball_src_dir}"/src_c/stem_UTF_8_*.h "${generated_dir}/src_c/"

c_count="$(find "${generated_dir}/src_c" -name 'stem_UTF_8_*.c' | wc -l)"
h_count="$(find "${generated_dir}/src_c" -name 'stem_UTF_8_*.h' | wc -l)"

if [[ "${c_count}" -eq 0 || "${c_count}" -ne "${h_count}" ]]; then
  echo "error: generated source/header count mismatch: ${c_count} .c, ${h_count} .h" >&2
  exit 1
fi

echo "Generated ${c_count} UTF-8 stemmer source/header pairs."
echo "Done. Review and commit ${generated_dir}."
