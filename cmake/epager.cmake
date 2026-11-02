# Shared build helpers for our own ESP-IDF components (never applied to IDF or managed components).

# Warning flags for our code in firmware builds. -Wpedantic is left out here because ESP-IDF's own
# headers use #include_next; the portable code is still compiled with it by tests/host and
# tools/xtensa_check.sh.
function(epager_component_warnings)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)
endfunction()
