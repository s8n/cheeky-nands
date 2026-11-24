#!/bin/sh
# Do the host test suites have teeth? Applies deliberate bugs ("mutations"), one at a time, to
# a COPY of source/bis.c (against test/run.sh's checks), of source/main.c (against the
# scenarios of test/ui_test.sh) or of source/net.c (against the transport checks of
# test/transport_test.sh), and requires the suite to fail for every one of them.
#
# The unmutated engine is built and run first, with exactly the flags the mutants get. If it
# does not pass, nothing below would mean anything and the script stops.
#
# Needs what test/run.sh needs, plus uv (or PYTHON=python3) for test/mutate.py.
#
#   exit 0  every mutation was caught
#   exit 1  at least one mutation passed the suite: the suite is missing a case
#   exit 2  the baseline did not pass, or a mutation is invalid (its target text is not in
#           its source file exactly once, or the mutant does not compile): this script needs
#           updating
set -u
cd "$(dirname "$0")/.."
: "${CC:=cc}"
mkdir -p build-host build-host/mut
. ./test/host_env.sh
# shellcheck disable=SC2086
MUT_CFLAGS="-std=c11 -Wall -Wextra -g -O1 $SAN $NBE_HOST_CFLAGS -DNBE_TEST_TMPDIR=\"$PWD/build-host/mut/tmp\""
# shellcheck disable=SC2086
MUTATE="${PYTHON:-uv run --no-project --quiet python} test/mutate.py"

# Build $1 as bis.c and run the suite. Exit status: the suite's, or 99 when the build failed.
build_and_run() {
    rm -rf build-host/mut/tmp build-host/mut/test_bis
    # shellcheck disable=SC2086
    $CC $MUT_CFLAGS "$1" source/nbe_fmt.c source/net.c test/shim/shim.c test/test_bis.c \
        -o build-host/mut/test_bis 2> build-host/mut/cc.log || return 99
    timeout 120 ./build-host/mut/test_bis > build-host/mut/out.txt 2>&1
}

echo "baseline: the unmutated engine with the mutants' flags"
if build_and_run source/bis.c; then
    echo "  passed: $(tail -n 1 build-host/mut/out.txt)"
else
    rc=$?
    echo "  DID NOT PASS, so no mutation result below would mean anything:"
    if [ "$rc" -eq 99 ]; then
        sed 's/^/    /' build-host/mut/cc.log | tail -n 15
    else
        grep "FAIL" build-host/mut/out.txt | head -n 15 | sed 's/^/    /'
        echo "    $(tail -n 1 build-host/mut/out.txt)"
    fi
    rm -rf build-host/mut
    exit 2
fi

# The console UI: source/main.c is compiled apart (it holds main(), and so does the test) and
# run through every scenario of test/ui_test.sh.
# shellcheck disable=SC2086
MUT_UI_CFLAGS="$MUT_CFLAGS -Itest/switch -DNBE_SD_MOUNT=\"$PWD/build-host/mut/sdcard\""
build_and_run_ui() { # $1 = main.c: 0 when every scenario passes, else its status, or 99
    rm -rf build-host/mut/sdcard build-host/mut/images build-host/mut/main_ui.o build-host/mut/test_ui
    # shellcheck disable=SC2086
    $CC $MUT_UI_CFLAGS -Dmain=nbe_app_main -c "$1" -o build-host/mut/main_ui.o 2> build-host/mut/cc.log || return 99
    # shellcheck disable=SC2086
    $CC $MUT_UI_CFLAGS source/bis.c source/nbe_fmt.c test/shim/shim.c test/ui_shim.c \
        test/net_stub.c test/test_ui.c build-host/mut/main_ui.o -o build-host/mut/test_ui \
        2> build-host/mut/cc.log || return 99
    for s in $(build-host/mut/test_ui --list); do
        rm -rf build-host/mut/sdcard build-host/mut/images # a fresh card per scenario, as in ui_test.sh
        timeout 120 ./build-host/mut/test_ui "$s" "$PWD/build-host/mut" > build-host/mut/out.txt 2>&1 \
            || return $?
    done
    return 0
}

# The transport of the network reader (source/net.c) with its socket calls scripted: what is
# built in test/transport_test.sh.
build_and_run_transport() { # $1 = net.c: 0 when the checks pass, else their status, or 99
    rm -f build-host/mut/test_transport
    # shellcheck disable=SC2086
    $CC $MUT_CFLAGS "$1" source/bis.c source/nbe_fmt.c test/shim/shim.c test/test_transport.c \
        -Wl,--wrap=poll -Wl,--wrap=send -Wl,--wrap=recv -Wl,--wrap=fcntl \
        -o build-host/mut/test_transport 2> build-host/mut/cc.log || return 99
    timeout 120 ./build-host/mut/test_transport > build-host/mut/out.txt 2>&1
}

echo "baseline: the unmutated UI, every scenario"
if build_and_run_ui source/main.c; then
    echo "  passed"
else
    rc=$?
    echo "  DID NOT PASS, so no UI mutation result below would mean anything:"
    if [ "$rc" -eq 99 ]; then
        sed 's/^/    /' build-host/mut/cc.log | tail -n 15
    else
        sed 's/^/    /' build-host/mut/out.txt | head -n 15
    fi
    rm -rf build-host/mut
    exit 2
fi

echo "baseline: the unmutated transport, with its socket calls scripted"
if build_and_run_transport source/net.c; then
    echo "  passed: $(tail -n 1 build-host/mut/out.txt)"
else
    rc=$?
    echo "  DID NOT PASS, so no transport mutation result below would mean anything:"
    if [ "$rc" -eq 99 ]; then
        sed 's/^/    /' build-host/mut/cc.log | tail -n 15
    else
        grep "FAIL" build-host/mut/out.txt | head -n 15 | sed 's/^/    /'
        echo "    $(tail -n 1 build-host/mut/out.txt)"
    fi
    rm -rf build-host/mut
    exit 2
fi

caught=0; missed=0; invalid=0

# Why a mutant failed, in one line: its first failed check, or else what stopped the run (a
# mutant can crash before a check reports it: the sanitizers are part of the suite).
caught_because() {
    why=$(grep -m 1 'FAIL' build-host/mut/out.txt | sed 's/^ *//; s/^[^:]*:[0-9]*: //')
    if [ -n "$why" ]; then
        printf '%s' "$why"
        return
    fi
    grep -m 1 -E 'ERROR: [A-Za-z]*Sanitizer|runtime error|Segmentation fault|Aborted' build-host/mut/out.txt \
        || printf 'it failed without a message'
}

# mutate <name>: the mutation (original text, a line ===>, replacement) on stdin.
mutate() {
    printf "  %-58s " "$1"
    # shellcheck disable=SC2086
    if ! why=$($MUTATE source/bis.c build-host/mut/bis.c 2>&1); then
        echo "INVALID: $why"
        invalid=$((invalid + 1))
        return
    fi
    build_and_run build-host/mut/bis.c
    rc=$?
    if [ "$rc" -eq 99 ]; then
        echo "INVALID: the mutant does not compile: $(grep -m 1 'error:' build-host/mut/cc.log)"
        invalid=$((invalid + 1))
    elif [ "$rc" -eq 124 ]; then
        echo "caught: it did not end by itself, and was stopped after two minutes"
        caught=$((caught + 1))
    elif [ "$rc" -eq 0 ]; then
        echo "NOT CAUGHT"
        missed=$((missed + 1))
    else
        echo "caught: $(caught_because)"
        caught=$((caught + 1))
    fi
}

# ---- planning ---------------------------------------------------------------------------------
mutate "part count rounds down" <<'M'
    return (int)(s / BIS_PART_SIZE + (s % BIS_PART_SIZE != 0));
===>    return (int)(s / BIS_PART_SIZE);
M
mutate "a full part is one byte too long" <<'M'
    return rest > (s64)BIS_PART_SIZE ? (s64)BIS_PART_SIZE : rest;
===>    return rest > (s64)BIS_PART_SIZE ? (s64)BIS_PART_SIZE + 1 : rest;
M
mutate "a full part is one byte too short" <<'M'
    return rest > (s64)BIS_PART_SIZE ? (s64)BIS_PART_SIZE : rest;
===>    return rest > (s64)BIS_PART_SIZE ? (s64)BIS_PART_SIZE - 1 : rest;
M
mutate "dump planned from the scan-time size, not the storage's" <<'M'
    bis_plan(&plan, out_dir, stem, rng_length, split);
    if (plan.parts > BIS_MAX_PART_FILES) {
===>    bis_plan(&plan, out_dir, stem, part->size, split);
    if (plan.parts > BIS_MAX_PART_FILES) {
M
mutate "a negative storage length is accepted" <<'M'
    if (R_SUCCEEDED(r->rc) && source_size < 0) r->rc = NBE_RC_BAD_SIZE;
===>    (void)0;
M

# ---- the copy loop ----------------------------------------------------------------------------
mutate "each chunk skips a byte of the source" <<'M'
            off += (s64)n;
            done += (s64)n;
===>            off += (s64)n + 1;
            done += (s64)n;
M
mutate "part base offset ignores the part index" <<'M'
        const s64 base = plan.multi ? (s64)i * (s64)BIS_PART_SIZE : 0;
===>        const s64 base = 0;
M
mutate "part suffix reuses index 0 for every part" <<'M'
    if (plan->multi) with_part_suffix(out, plan->base, index);
===>    if (plan->multi) with_part_suffix(out, plan->base, 0);
M
mutate "part files are created empty instead of pre-sized" <<'M'
        r->rc = fsFsCreateFile(sd, path, len, 0);
===>        r->rc = fsFsCreateFile(sd, path, 0, 0);
M
mutate "read error does not stop the dump" <<'M'
            r->rc = io_read(&st, rng_start + base + off, g_chunk, n);
            if (R_FAILED(r->rc)) {
===>            r->rc = io_read(&st, rng_start + base + off, g_chunk, n);
            if (false) {
M
mutate "write error does not stop the dump" <<'M'
            r->rc = io_write(&f, off, g_chunk, n);
            if (R_FAILED(r->rc)) {
===>            r->rc = io_write(&f, off, g_chunk, n);
            if (false) {
M
mutate "cancel is ignored" <<'M'
                if (!cb->progress(cb->user, &pg)) {
===>                if (!cb->progress(cb->user, &pg) && false) {
M
mutate "bounce fallback disabled for reads" <<'M'
    Result rc = fsStorageRead(st, off, buf, n);
    if (R_VALUE(rc) != RC_BUFFER_UNUSABLE) return rc;
===>    Result rc = fsStorageRead(st, off, buf, n);
    return rc;
M
mutate "bounce fallback disabled for writes" <<'M'
    Result rc = fsFileWrite(f, off, buf, n, FsWriteOption_None);
    if (R_VALUE(rc) != RC_BUFFER_UNUSABLE) return rc;
===>    Result rc = fsFileWrite(f, off, buf, n, FsWriteOption_None);
    return rc;
M

# ---- hashes -----------------------------------------------------------------------------------
mutate "whole-image hash never updated" <<'M'
            sha256ContextUpdate(&whole, g_chunk, n);
===>            g_chunk[0] = g_chunk[0];
M
mutate "per-part hash uses the whole-image context" <<'M'
        sha256ContextGetHash(&pc, r->part_sha256[i]);
===>        sha256ContextGetHash(&whole, r->part_sha256[i]);
M
mutate "per-part hash covers only the last chunk" <<'M'
            sha256ContextUpdate(&pc, g_chunk, n);
===>            sha256ContextCreate(&pc);
            sha256ContextUpdate(&pc, g_chunk, n);
M

# ---- side files and verification --------------------------------------------------------------
mutate "text files opened without the Append bit" <<'M'
FsOpenMode_Write | FsOpenMode_Append, &w->f);
===>FsOpenMode_Write, &w->f);
M
mutate "no length verification of the part files" <<'M'
        if (got != want) {
===>        if (false && got != want) {
M
mutate "plaintext detection inverted" <<'M'
        return b[0] == 0xEB || b[0] == 0xE9 || (b[0x1FE] == 0x55 && b[0x1FF] == 0xAA);
===>        return !(b[0] == 0xEB || b[0] == 0xE9 || (b[0x1FE] == 0x55 && b[0x1FF] == 0xAA));
M

# ---- replacing an earlier dump ----------------------------------------------------------------
mutate "earlier dump removed before the partition is open" <<'M'
    r->stage = "open";
    r->rc = fsOpenBisStorage(&st, (FsBisPartitionId)part->id);
===>    (void)bis_delete_outputs(sd, out_dir, part->name);
    r->stage = "open";
    r->rc = fsOpenBisStorage(&st, (FsBisPartitionId)part->id);
M
mutate "existing outputs not replaced" <<'M'
    r->stage = "replace";
    r->rc = bis_delete_outputs(sd, out_dir, stem);
===>    r->stage = "replace";
    r->rc = 0;
M
mutate "cleanup also runs for refused dumps (costs the old backup)" <<'M'
    if (!ok && replacing) {
===>    if (!ok) {
M
mutate "no cleanup after a failed dump" <<'M'
    if (!ok && replacing) {
===>    if (!ok && replacing && false) {
M
mutate "replace flag set although the old dump could not be removed" <<'M'
        setmsg(r->message, sizeof r->message, "removing the previous %s dump failed", part->name);
        goto cleanup;
    }
    replacing = true; // from here on the new dump owns the name
===>        setmsg(r->message, sizeof r->message, "removing the previous %s dump failed", part->name);
        replacing = true;
        goto cleanup;
    }
M
mutate "deletion errors are swallowed" <<'M'
    return run_walk(sd, out_dir, name, true, false).error;
===>    (void)run_walk(sd, out_dir, name, true, false);
    return 0;
M
mutate "outputs_exist always says no" <<'M'
    return run_walk(sd, out_dir, name, false, false).any;
===>    return run_walk(sd, out_dir, name, false, false).any && false;
M
mutate "output walk skips the un-split file" <<'M'
    for (int i = 0; i < 3 + BIS_MAX_PART_FILES; i++) {
===>    for (int i = 1; i < 3 + BIS_MAX_PART_FILES; i++) {
M

# ---- scanning ---------------------------------------------------------------------------------
mutate "scan skips unknown partition ids" <<'M'
        if (!ok && !k) continue; // nothing here and nothing named: not worth a row
===>        // mutated: unknown ids are skipped
M
mutate "scan drops partitions it cannot open" <<'M'
        if (!ok && !k) continue; // nothing here and nothing named: not worth a row
===>        if (!ok) continue;
M
mutate "scan accepts a negative storage length" <<'M'
            if (R_SUCCEEDED(rc) && size < 0) rc = NBE_RC_BAD_SIZE;
===>            if (R_SUCCEEDED(rc) && size < 0) rc = 0;
M
mutate "scan does not report a truncated list" <<'M'
        if (t->count >= BIS_MAX_PARTITIONS) { t->truncated = true; break; }
===>        if (t->count >= BIS_MAX_PARTITIONS) { break; }
M
mutate "a Switch 2 lists the ids it has no storage for" <<'M'
        if (!ok && switch2 && bis_absent_on_switch2(id)) continue; // no such storage on a Switch 2
===>        // mutated: every named id gets a row
M
mutate "the absent-on-Switch-2 list starts at 22" <<'M'
    21, 22, 23, 24, 25, 26, // BootConfigAndPackage2Part1..6
===>    22, 23, 24, 25, 26, // BootConfigAndPackage2Part1..6
M
mutate "the absent-on-Switch-2 list also hides the user data area" <<'M'
    21, 22, 23, 24, 25, 26, // BootConfigAndPackage2Part1..6
===>    20, 21, 22, 23, 24, 25, 26, // BootConfigAndPackage2Part1..6
M

# ---- the on-console 4 GiB experiment ----------------------------------------------------------
mutate "4GiB test trusts CreateFile without reading the length back" <<'M'
    out->accepts = R_SUCCEEDED(out->create_rc) && R_SUCCEEDED(out->reopen_rc) &&
                   out->stored_size == size;
===>    out->accepts = R_SUCCEEDED(out->create_rc);
M
mutate "4GiB test leaves its file behind" <<'M'
    out->delete_rc = fsFsDeleteFile(sd, path);
===>    out->delete_rc = 0;
M

# ---- ranges --------------------------------------------------------------------------------------

mutate "a range dump ignores where the range starts" <<'M'
            r->rc = io_read(&st, rng_start + base + off, g_chunk, n);
===>            r->rc = io_read(&st, base + off, g_chunk, n);
M
mutate "a partial dump is named like the whole partition" <<'M'
        bis_partial_stem(stem, sizeof stem, part->name, rng_start, rng_start + rng_length);
===>        snprintf(stem, sizeof stem, "%s", part->name);
M
mutate "a range need not be a multiple of 512" <<'M'
        if (start < 0 || (start & 511) || length < 0 || (length & 511)) {
===>        if (start < 0 || length < 0) {
M
mutate "a start beyond the end of the partition is accepted" <<'M'
    if (!is_whole && rng_start >= source_size) { // a whole dump of an empty partition is fine
===>    if (false && !is_whole && rng_start >= source_size) { // a whole dump of an empty partition is fine
M

# ---- the network reader ------------------------------------------------------------------------

mutate "READ over the network ignores where the range starts" <<'M'
        rc = io_read(&st, r->start + sent, g_chunk, n);
===>        rc = io_read(&st, sent, g_chunk, n);
M
mutate "the hash sent over the network is not of the bytes sent" <<'M'
        sha256ContextUpdate(&sha, g_chunk, n);
        if (sending) {
===>        (void)0;
        if (sending) {
M
mutate "LIST leaves out the partitions that cannot be opened" <<'M'
        const BisPartition* p = &t->parts[i];
        snprintf(line, sizeof line, "PART %u %d %lld %08x %s\n", p->id, p->openable ? 1 : 0,
===>        const BisPartition* p = &t->parts[i];
        if (!p->openable) continue;
        snprintf(line, sizeof line, "PART %u %d %lld %08x %s\n", p->id, p->openable ? 1 : 0,
M
mutate "a refused request closes the connection" <<'M'
refused:
    (void)link_err(link, rc, r->stage);
    r->rc = rc;
    r->refused = true;
    r->elapsed_ns = bis_now_ns() - t0;
    return true;
===>refused:
    (void)link_err(link, rc, r->stage);
    r->rc = rc;
    r->refused = true;
    r->elapsed_ns = bis_now_ns() - t0;
    return false;
M
mutate "a refusal is forgotten when the reader closes the connection" <<'M'
            if (r->request[0] == '\0') r->stage = "done";
            return;
===>            r->rc = 0;
            r->stage = "done";
            return;
M
mutate "the result does not say that the request was refused" <<'M'
refused:
    (void)link_err(link, rc, r->stage);
    r->rc = rc;
    r->refused = true;
===>refused:
    (void)link_err(link, rc, r->stage);
    r->rc = rc;
M
mutate "a length that is not a decimal number is read as 0, to the end" <<'M'
        ok = take_number(&p, &length);
        take_spaces(&p);
    }
    if (!ok || *p != '\0' || id > 0xFFFFFFFFULL || start > 0x7FFFFFFFFFFFFFFFULL) {
===>        (void)take_number(&p, &length);
        while (*p != '\0') p++;
    }
    if (!ok || id > 0xFFFFFFFFULL || start > 0x7FFFFFFFFFFFFFFFULL) {
M
mutate "a length too large for 64 bits sends nothing" <<'M'
    if (r->length <= 0 || r->length > size - r->start) r->length = size - r->start;
===>    if (r->length == 0 || r->length > size - r->start) r->length = size - r->start;
M

# ---- the console UI ---------------------------------------------------------------------------

# mutate_ui <name>: the same, for source/main.c against the UI scenarios.
mutate_ui() {
    printf "  %-58s " "$1"
    # shellcheck disable=SC2086
    if ! why=$($MUTATE source/main.c build-host/mut/main.c 2>&1); then
        echo "INVALID: $why"
        invalid=$((invalid + 1))
        return
    fi
    build_and_run_ui build-host/mut/main.c
    rc=$?
    if [ "$rc" -eq 99 ]; then
        echo "INVALID: the mutant does not compile: $(grep -m 1 'error:' build-host/mut/cc.log)"
        invalid=$((invalid + 1))
    elif [ "$rc" -eq 124 ]; then
        echo "caught: it did not end by itself, and was stopped after two minutes"
        caught=$((caught + 1))
    elif [ "$rc" -eq 0 ]; then
        echo "NOT CAUGHT"
        missed=$((missed + 1))
    else
        echo "caught: $(caught_because)"
        caught=$((caught + 1))
    fi
}

mutate_ui "free-space check ignores the dump that gets replaced" <<'M'
    return g_free + (replaced > 0 ? replaced : 0) >= need;
===>    return g_free >= need;
M
mutate_ui "ALL goes on after a failure that cost an old dump" <<'M'
        if (!failed_before_replacing(&g_dump)) {
===>        if (false && !failed_before_replacing(&g_dump)) {
M
mutate_ui "'+' in the cancel dialog does not exit the app" <<'M'
    if (a < 0) g_quit = true; // '+' means exit, also from the middle of what the app is doing
===>    if (false) g_quit = true; // '+' means exit, also from the middle of what the app is doing
M
mutate_ui "result code in a list row is cut short" <<'M'
                char rs[64]; // "0x00320002 PermissionDenied" does not fit a size buffer
===>                char rs[24]; // "0x00320002 PermissionDenied" does not fit a size buffer
M
mutate_ui "a long name is cut in a row that opens" <<'M'
                snprintf(line, sizeof line, "%3u] %-31.31s %10s  %s", p->id, p->name, sz,
===>                snprintf(line, sizeof line, "%3u] %-30.30s %10s  %s", p->id, p->name, sz,
M
mutate_ui "a long name is cut in a row that cannot be opened" <<'M'
                snprintf(line, sizeof line, "%3u] %-31.31s %10s  %s", p->id, p->name, "-", rs);
===>                snprintf(line, sizeof line, "%3u] %-30.30s %10s  %s", p->id, p->name, "-", rs);
M
mutate_ui "the list does not scroll to keep the selection in view" <<'M'
    if (g_sel >= g_top + h) g_top = g_sel - h + 1;
===>    if (false && g_sel >= g_top + h) g_top = g_sel - h + 1;
M
mutate_ui "a failure is logged without its result code" <<'M'
            ui_log("%s: FAILED at %s [%s]", who, r->stage, rs);
===>            ui_log("%s: FAILED at %s", who, r->stage);
M

mutate_ui "a partial dump ignores the chosen end" <<'M'
    range.start = 0;
    range.length = end;
===>    range.start = 0;
    range.length = 0;
M
mutate_ui "the end can be moved past the end of the partition" <<'M'
                if (e > p->size) e = p->size; // the whole partition
                else if (e < 512) e = 512;    // one block
===>                if (e < 512) e = 512;      // one block (mutant: no upper limit)
M

mutate_ui "holding a move key does not repeat" <<'M'
            } else if (held & repeat_key) {
                if (now >= repeat_at) { // held: repeat
===>            } else if (false && held & repeat_key) {
                if (now >= repeat_at) { // held: repeat
M
mutate_ui "the repeat has no delay" <<'M'
                repeat_at = now + (repeating ? REPEAT_INTERVAL_NS : REPEAT_DELAY_NS);
===>                repeat_at = now + REPEAT_INTERVAL_NS;
M

mutate_ui "what was sent over the network is not logged" <<'M'
        ui_log("network: %s: sent %s in %s (%s/s)", r->request, sz, hms, spd);
===>        ;
M
mutate_ui "a refused request is logged as answered" <<'M'
        ui_log("network: %s: refused at %s [%s]", r->request, r->stage, rs);
        return;
===>        ;
        return;
M
mutate_ui "a reader that goes silent is never let go of" <<'M'
    if (idle_ns >= NET_STALL_NS) {
        g_net_let_go = "it went silent";
        return false;
    }
===>    (void)0;
M
mutate_ui "waiting for a reader is not shown on the screen" <<'M'
    if (idle_ns / SECOND_NS != shown_second) { // the screen shows whole seconds
        shown_second = idle_ns / SECOND_NS;
        draw_waiting(r, idle_ns);
    }
===>    (void)0;
M
mutate_ui "B does not let a waiting reader go" <<'M'
        if (ask_stop(lines, "let go") != 0) {
            g_net_let_go = "you let it go";
            return false;
        }
===>        (void)ask_stop(lines, "let go");
M
mutate_ui "a reader that goes silent is not logged as let go of" <<'M'
        ui_log("network: a reader connected and sent nothing for %s: let go", hms);
===>        ;
M
mutate_ui "the log does not say how much had gone through when a reader was let go of" <<'M'
        ui_log("network: %s: the reader took nothing for %s: let go after %s", r->request, hms, sz);
===>        ui_log("network: %s: the reader took nothing for %s: let go after %s", r->request, hms, "?");
M
mutate_ui "letting a reader go by hand is not logged" <<'M'
    if (strcmp(why, "it went silent") != 0) {
        ui_log("network: connection let go (%s)", why);
===>    if (strcmp(why, "it went silent") != 0) {
        ;
M
mutate_ui "sockets that cannot be made non-blocking are not reported" <<'M'
        ui_log("network: this console's sockets cannot be made non-blocking: a reader that goes "
               "silent can keep the app until it is restarted");
===>        ;
M
mutate_ui "the console is not asked not to sleep" <<'M'
    const Result sleep_rc = appletSetAutoSleepDisabled(true);
===>    const Result sleep_rc = 0;
M
mutate_ui "the console's agreement to stay awake is not logged" <<'M'
        ui_log("the console will not fall asleep on its own while this app runs");
===>        ;
M
mutate_ui "a console that will not stay awake is not reported" <<'M'
        ui_log("the console could not be asked not to sleep (%s): keep it awake during a long transfer",
               rs);
===>        (void)rs;
M

# ---- the network reader's transport -----------------------------------------------------------
# mutate_net <name>: the same, for source/net.c against the transport checks.
mutate_net() {
    printf "  %-58s " "$1"
    # shellcheck disable=SC2086
    if ! why=$($MUTATE source/net.c build-host/mut/net.c 2>&1); then
        echo "INVALID: $why"
        invalid=$((invalid + 1))
        return
    fi
    build_and_run_transport build-host/mut/net.c
    rc=$?
    if [ "$rc" -eq 99 ]; then
        echo "INVALID: the mutant does not compile: $(grep -m 1 'error:' build-host/mut/cc.log)"
        invalid=$((invalid + 1))
    elif [ "$rc" -eq 124 ]; then
        echo "caught: it did not end by itself, and was stopped after two minutes"
        caught=$((caught + 1))
    elif [ "$rc" -eq 0 ]; then
        echo "NOT CAUGHT"
        missed=$((missed + 1))
    else
        echo "caught: $(caught_because)"
        caught=$((caught + 1))
    fi
}

mutate_net "the socket is not made non-blocking" <<'M'
    return fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ? NBE_RC_NET : 0;
===>    return 0;
M
mutate_net "the idle hook is never asked while the socket makes us wait" <<'M'
        if (c->idle && !c->idle(c->idle_user, bis_now_ns() - c->progress_ns)) return 0;
===>        (void)0;
M
mutate_net "the idle hook's giving up is ignored" <<'M'
        if (c->idle && !c->idle(c->idle_user, bis_now_ns() - c->progress_ns)) return 0;
===>        (void)(c->idle && !c->idle(c->idle_user, bis_now_ns() - c->progress_ns));
M
mutate_net "the idle time is not started again by a byte that is sent" <<'M'
        p += (size_t)n;
        len -= (size_t)n;
        c->progress_ns = bis_now_ns();
===>        p += (size_t)n;
        len -= (size_t)n;
M
mutate_net "the idle time is not started again by a byte of the reader" <<'M'
        if (got == 0) return n == 0 ? 0 : -1; // the reader closed the connection
        c->progress_ns = bis_now_ns();
===>        if (got == 0) return n == 0 ? 0 : -1; // the reader closed the connection
M
mutate_net "a partial send loses what the socket did not take" <<'M'
        p += (size_t)n;
===>        p += piece;
M
mutate_net "a close in the middle of a line passes for the end of the line" <<'M'
        if (got == 0) return n == 0 ? 0 : -1; // the reader closed the connection
===>        if (got == 0) return 0;
M
mutate_net "a line is cut one byte short" <<'M'
        if (n + 1 >= cap) return -1;
===>        if (n + 2 >= cap) return -1;
M
mutate_net "the wait to send is for something to read" <<'M'
            if (wait_ready(c, POLLOUT) != 1) return NBE_RC_NET; // the reader takes nothing
===>            if (wait_ready(c, POLLIN) != 1) return NBE_RC_NET; // the reader takes nothing
M
mutate_net "a poll() that fails is taken for the socket being ready" <<'M'
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
===>        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
M
mutate_net "an interrupted send is taken for a broken connection" <<'M'
            if (errno == EINTR) continue;
            if (!nothing_moved()) return NBE_RC_NET;
===>            if (!nothing_moved()) return NBE_RC_NET;
M
mutate_net "ENOBUFS is taken for a broken connection" <<'M'
static bool nothing_moved(void) { return errno == EAGAIN || errno == ENOBUFS; }
===>static bool nothing_moved(void) { return errno == EAGAIN; }
M

rm -rf build-host/mut
echo "mutations: $caught caught, $missed not caught, $invalid invalid"
[ "$invalid" -ne 0 ] && exit 2
[ "$missed" -ne 0 ] && exit 1
exit 0
