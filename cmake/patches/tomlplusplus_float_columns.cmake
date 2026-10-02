# Run from the tomlplusplus source dir as its FetchContent PATCH_COMMAND.
#
# toml++ 3.4.0 estimates a float's printed width as
# static_cast<size_t>(log10(val)) + 1. For |val| < 1 the log is negative, and
# converting a negative double to size_t is undefined: UBSan aborts, and without
# it the estimate (and so where arrays wrap) differs between x86 and arm64.
# Count one integer digit for those values instead. Idempotent.
set(file "include/toml++/impl/toml_formatter.inl")
file(READ "${file}" text)
set(old "return weight + static_cast<size_t>(log10(val)) + 1u;")
set(new "return weight + (val >= 1.0 && val < 1e308 ? static_cast<size_t>(log10(val)) : size_t{}) + 1u;")
string(FIND "${text}" "${new}" patched)
if(patched EQUAL -1)
  string(FIND "${text}" "${old}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "tomlplusplus patch: expected line not found in ${file}")
  endif()
  string(REPLACE "${old}" "${new}" text "${text}")
  file(WRITE "${file}" "${text}")
endif()
