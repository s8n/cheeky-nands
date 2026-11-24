# cheeky-nands (with the lads)

A homebrew NRO that reads the console's NAND partitions and hands them out
either as files on the SD card, or over the network to a PC. It only reads:
the app never writes to NAND. Everything it writes goes to one folder on the SD
card.

There are two ways to use it:

- **On the console**: pick a partition and dump it to `sdmc:/cheeky-nands/`.
- **Over the network**: turn on the network reader, then pull partition bytes
  with `net_pull.py` from a PC, and check a dump against the console afterwards.

## Running it

Copy the NRO for your console to the SD card and start it from the homebrew
menu:

| Console | NRO |
|---|---|
| Switch 2 | `cheeky-nands-switch2.nro` |
| Switch 1 | `cheeky-nands.nro` |

To build them yourself, run `./build.sh` (Switch 2, the default) or
`./build.sh switch` (Switch 1). Both need Docker and the matching image.

At startup the app asks the console for every partition id and lists what it
gets. `Y` scans again. Partitions the process may not open are still listed,
with the result code in place of a size (for example
`0x00000E02 TargetLocked`). On a Switch 2, `PRODINFOF`, `USER`, `SYSTEM` and
`SystemProperPartition` are refused this way; on a Switch 1 they open.

Ids that a Switch 2 has no storage for at all are left out instead: 21 to 26,
29, 32, 34, 36 and 39. They are not in the list, `ALL` skips them, and a PC's
`list` does not show them either. A PC can still ask for one by id; the console
answers with the result code.

## Dumping on the console

| Button | Does |
|---|---|
| Up / Down | Move the selection. The first row, `ALL`, means every partition that can be opened. |
| A | Dump the selection. A dialog shows what will be written and how much space the card has. |
| B | Go back in a dialog. During a dump: ask whether to cancel. While a PC is connected: ask whether to drop it. |
| X | Toggle splitting into parts of just under 4 GiB. On by default. |
| Y | Scan the partitions again. |
| ZL | Dump the selected partition from its start up to an end you choose. |
| ZR | Turn the network reader on and off (see [Pulling over the network](#pulling-over-the-network)). |
| + / − | Exit. While a dump runs or a PC is connected, only `+` does. |

The app keeps the console from falling asleep while it runs.

### Output files

For a partition called `Name`, output goes to `sdmc:/cheeky-nands/`:

| File | Content |
|---|---|
| `Name.bin` | The partition image. With splitting on, `Name.bin.00`, `Name.bin.01`, ... |
| `Name.sha256` | One line per file, in `sha256sum -c` format, plus the hash of the whole partition. |
| `Name.info` | Key = value text: partition id and name, size, part count, SHA-256, and whether the dump looks plaintext. |
| `log.txt` | Everything the log area on screen showed, appended on each run. |

Splitting matters because a FAT32 card cannot hold a single file over 4 GiB.
On an exFAT card `X` can be switched off to write one file per partition; on
FAT32 leave it on.

Put the parts back together on a PC:

```
cat User.bin.[0-9][0-9] > User.bin
sha256sum -c User.sha256              # checks each part
cat User.bin.[0-9][0-9] | sha256sum   # compare with the "# whole partition" line
```

A dump replaces an earlier dump of the same partition in place: the old parts,
`.sha256` and `.info` are deleted first. If a dump fails or is cancelled after
that point, the old dump is gone and no half-written files are left behind. The
dialog warns you when an earlier dump exists.

In `ALL`, a partition that cannot be opened is skipped; the first real failure
stops the run, so a full card does not cost every following partition its
earlier dump.

### Partial dumps

`ZL` dumps from the start of the partition up to an end you choose. Left/Right
move the end by a 32nd of the partition (32 MiB at most); L/R by a quarter
(1 GiB at most). `A` starts, `B` goes back. The result is
`Name+0x<start>-0x<end>.bin`, with both numbers in hex, `<end>` exclusive. It
only ever replaces files of exactly that name, so a full dump of the same
partition is safe.

The end is chosen by hand; the app does not look for a partition table to
suggest one.

### About the bytes

`OpenBisStorage` returns partitions already decrypted where the FS sysmodule
decrypts them, so a dump from this tool is plain, not a raw XTS image. The
exception is `UserDataRoot` on a Switch 2, which is encrypted in full. Each
dump's `.info` has `looks_plaintext` (`1` plain, `0` expected a signature and
did not find one, `-1` not checked) so a dump cannot be mistaken for a raw one.

## Pulling over the network

`ZR` turns the console into a TCP server on port 28900. A PC then asks for
whatever bytes it wants; nothing needs to be set on the console. A transfer
writes no dump file on the console, only log lines. The console keeps reading
NAND only.

Nothing checks who connects: while the reader is on, anything that can reach
the console can read every partition the app may read. Turn it off with `ZR`
when done.

The client is `net_pull.py`; it needs Python 3 and nothing else.

```
uv run --no-project python net_pull.py ping
uv run --no-project python net_pull.py list
uv run --no-project python net_pull.py list --out partitions.txt
uv run --no-project python net_pull.py read 20 0 end UserDataRoot.bin
```

`10.69.68.225` is the project Switch 2 and the default host; pass `--host` for
another console. `list` prints the partition number, whether it opens, its
size, the result code if refused, and the name.

`read` takes the partition id as `list` prints it, the first byte, and a length
(decimal or `0x` hex; `end` or `0` for the rest of the partition), then the
file to write. There is no 4 GiB limit on the PC side.

Every range the console sends comes with a SHA-256, which the client checks. If
a transfer breaks off, it asks again from what it already has, up to
`--retries` (default 5). A file left by an earlier run stops the read unless
`--resume` is given, which continues from its end; `--force` starts over.
`<file>.sha256` is written next to the file for `sha256sum -c`.

A partition the console refuses comes back as `the console refused: ERR ...`
with the console's result code.

While a PC is connected the console shows what was asked for. `B` drops the
connection, `+` exits. If the PC goes silent for two minutes the console drops
it by itself and listens again. `nc` is enough for a one-off request:

```
printf 'LIST\n' | nc -N 10.69.68.225 28900
```

A full pull of `UserDataRoot` (255.5 GB) took about six hours at roughly
11 MiB/s.

## Validating over the network

`verify` has the console hash a range again, piece by piece, without sending the
bytes, and compares every piece with the file on the PC:

```
uv run --no-project python net_pull.py verify 20 0 end UserDataRoot.bin
```

With `end` as the length, the file's own length is the range to check. Pieces
are 256 MiB (`--piece` changes that). They are small on purpose: the console
keeps writing logs and saves to its storage, so a piece that differs may be
the console's own writes rather than a fault of the transfer. When a piece does
not match, `verify` reads that piece once more and compares its bytes with the
file's: a few changed blocks are console writes, a shifted or missing run of
megabytes is a fault. `--reread N` limits how many mismatching pieces get that
treatment (default 10, `0` disables it).

`--count N` checks only N pieces spread over the range, which is useful for a
quick check of a large dump.

To find out which areas the console writes to, run `verify` twice, some time
apart: a piece whose hash changes in between is one the console writes to.

`read` also accepts `--verify auto|yes|no` to run this whole-file check as part
of a pull: `auto` (the default) does it after a break or on a resumed file,
`yes` after every read, `no` never. It costs a second read of the whole range
on the console.
