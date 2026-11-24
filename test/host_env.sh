# Sourced by test/run.sh and test/mutation_check.sh; not a program of its own.
# Requires: the project root as the working directory, ${CC} set, and an existing build-host/.
# Sets:
#   SAN              "-fsanitize=address,undefined", or empty when $CC cannot build AND run
#                    such a program here
#   NBE_HOST_CFLAGS  the flags the normal build and the mutants share
SAN=""
echo 'int main(void){return 0;}' > build-host/probe.c
# shellcheck disable=SC2086
if $CC -fsanitize=address,undefined build-host/probe.c -o build-host/probe 2>/dev/null &&
    [ -x build-host/probe ] && ./build-host/probe 2>/dev/null; then
    SAN="-fsanitize=address,undefined"
fi
rm -f build-host/probe build-host/probe.c
NBE_HOST_CFLAGS="-D_POSIX_C_SOURCE=200809L -DNBE_HOST_TEST -DBIS_PART_SIZE=0x20000ULL -DBIS_CHUNK_SIZE=0x10000ULL -Isource -Itest/shim"
