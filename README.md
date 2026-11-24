# nand-backup-experiments

A homebrew NRO that lists the console's BIS (NAND) partitions the way the FS
sysmodule exposes them (`fsp-srv` command 12, `OpenBisStorage`), and copies the
one you pick, or all of them, to the SD card. It can also send partition bytes
to a PC over the network. That is called the network reader here; see
[Over the network](#over-the-network).

It only reads NAND. The only `IStorage` commands it sends to a partition are
`GetSize` and `Read`; every write goes to one directory on the SD card. A
transfer to a PC writes no dump file, only lines in the log.

**Run on a console twice so far,** by hand: on a Switch 2 and on a Switch 1.
Both times it started and listed the partitions. On the Switch 2, `L` was
pressed as well. What the runs showed is written down once each:
[Permissions](#permissions) for which partitions opened, and
[Partial dumps](#partial-dumps) for what `L` found. On the Switch 2 the
network reader ran too, on 2026-11-23: it listed the partitions, pulled all
of `UserDataRoot` (255.5 GB, in about six hours), and checked that dump
against the console piece by piece. See
[Over the network](#over-the-network). No dump written to an SD card by the
app has been examined.
Apart from that, this page describes what the code is written to do. The section
[How far this has been checked](#how-far-this-has-been-checked) says what was tested,
and how.

## Build

```
./build.sh            # Switch 2 (default): nand-backup-experiments-switch2.nro, libounce
./build.sh switch     # Switch 1:         nand-backup-experiments.nro,      libnx 4.12.0
./build.sh clean           # removes the Switch 2 build
./build.sh switch clean    # removes the Switch 1 build
```

`build.sh` runs `make` inside a docker image (`ounce-sdk:latest` for Switch 2,
`devkitpro/devkita64:latest` for Switch 1), so it needs Docker and those images
on the machine. `ounce-sdk` is built from the folder `../kaiHEN/0.0.3/sdk/`
(`docker build -t ounce-sdk .` there). Both NROs land in this folder. Both builds are warning-free with
`-Wall -Wextra`, and both come from the same sources. libnx is the library Switch
homebrew is written against. libounce is "libnx with changes and additions made
for Switch 2" (`../libounce/README.md`) and has the same API as libnx for
everything this app calls.

## Use

Copy the NRO for your console to the SD card
(`nand-backup-experiments-switch2.nro` for a Switch 2,
`nand-backup-experiments.nro` for a Switch 1) and start it from the homebrew
menu. On a Switch 2, kaiHEN, a homebrew enabler, starts that menu when you
hold R while opening a game (`../civ7-fs-kaihen/README.md`).

| Button | Does |
|---|---|
| Up / Down | Move the selection. The first row, `ALL`, means every partition that can be opened. |
| A | Dump the selection. A dialog shows what will be written and how much space the card has. |
| B | In a dialog: go back. During a dump: ask whether to cancel. While a PC is connected over the network (`ZR`, see [Over the network](#over-the-network)): ask whether to drop its connection. |
| X | Toggle splitting of dumps larger than 4 GiB. It is on by default; see [The 4 GiB test](#the-4-gib-test) for when it can be off. |
| Y | Scan the partitions again. |
| R | Run the 4 GiB test. See [The 4 GiB test](#the-4-gib-test). |
| L | Show the partition table at the start of the selected partition. When there is none in plain text, that goes into the log and `Name.gpt.txt` is not written. See [Partial dumps](#partial-dumps). |
| ZL | Dump the selected partition from its start up to an end you choose. See [Partial dumps](#partial-dumps). |
| ZR | Turn the network reader on and off. A PC can then pull partition bytes. See [Over the network](#over-the-network). |
| + / − | Exit. While a dump runs or a PC is connected, only `+` does. |

The screen, as the test run on a PC shows it with three pretend partitions (on
a console the first line names the console and its firmware, and the log line
names the `sdmc:` path):

```
nand-backup-experiments v0.1  [host-test, fw 23.0.1]
-> sdmc:/nand-backup-experiments/  net: OFF
[A] dump ALL: 3 openable partition(s), 128+ KiB, over 4 GiB split
>  ALL    every openable partition    128+ KiB
   0] BootPartition1Root                 64+ KiB  BOOT0
  10] BootPartition2Root                       -  0x002EE202 InvalidArgument
   ...
  30] User                               64+ KiB  USER
   ...
  77] Unknown_77                           512 B

--- log, also appended to sdmc:/nand-backup-experiments/log.txt ---
---- nand-backup-experiments v0.1 (host-test, fw 23.0.1) ----
read-only towards NAND; output sdmc:/nand-backup-experiments/
the console will not fall asleep on its own while this app runs
scan: 21 partition(s) listed, 3 openable (128+ KiB in total)

[A] dump  [X] split >4GiB: ON   [Y] rescan  [L] GPT  [ZL] partial dump
[R] 4 GiB test  [ZR] network reader  [+] exit
```

Each row of the list has the partition's number, its name, its size (or `-`
and the result code it was refused with), and a last column. Output goes to
`sdmc:/nand-backup-experiments/`. Each partition is named as in libnx's
`FsBisPartitionId` (`User`, `System`, `BootPartition1Root`, ...), and the
numbers are decimal. One that opens but has no name there is listed as
`Unknown_` and its number. The last column is not read from the console. It
comes from a fixed list in the program: the partition's name in the Switch 1
partition table where it has one (`SYSTEM`, `SAFE`, `BCPKG2-1-Normal-Main`,
...), and the names they usually go by otherwise (`BOOT0`, `BOOT1`, `USER`,
`PRODINFO`, `PRODINFOF`). The two names are easy to mix up for the
calibration partitions: 28 is `CalibrationFile` to libnx and `PRODINFOF` in
that column, and 27 is `CalibrationBinary` and `PRODINFO`.

A partition the process may not open stays in the list with the result code
instead of a size, for example `0x00000E02 TargetLocked`.

## Output files

For a partition called `Name`:

| File | Content |
|---|---|
| `Name.bin`, or `Name.bin.00`, `Name.bin.01`, ... | The partition image. Split when it is larger than `0xFFFF8000` bytes (4 GiB minus 32 KiB) and splitting is on. |
| `Name.sha256` | One line per file, in `sha256sum -c` format. A comment line holds the hash of the whole partition. |
| `Name.info` | Key = value text, listed below. |
| `Name.gpt.txt` | The partition table at the start of the partition, as text. Written by `L`, not by a dump. |
| `log.txt` | Everything the log area on screen shows, appended to on each run. |

Put the parts back together on a PC:

```
cat User.bin.[0-9][0-9] > User.bin
sha256sum -c User.sha256              # checks each part
cat User.bin.[0-9][0-9] | sha256sum   # compare with the "# whole partition" line
```

The suffixes have two digits, so there can be up to 100 parts and they sort
correctly. A dump that was not split is a single `Name.bin`, and `Name.sha256`
lists that one file.

`Name.info` has these lines, in this order:

```
# written by nand-backup-experiments <version> (<platform>)
timestamp_unix=<seconds>
partition_id=<number>
partition_name=<name>
size=<bytes dumped>
start=<first byte dumped>                  (a partial dump only)
source_size=<size of the whole partition>  (a partial dump only)
parts=<number of files>
part_size_max=4294934528
sha256=<hex, of the bytes dumped>
looks_plaintext=<-1, 0 or 1>
```

`looks_plaintext` is explained under
[Plaintext or encrypted](#plaintext-or-encrypted).

## Re-dumping a partition

A dump replaces an earlier dump of the same partition in place. The order is:

1. The partition is opened and its size read. If this fails, nothing on the card
   is touched.
2. The files of the earlier dump are deleted: the parts, `Name.sha256` and
   `Name.info`. `Name.gpt.txt` and partial dumps of that partition are not among
   them.
3. The new files are written, each checked for length, and hashed.

If the dump fails or is cancelled after step 2, the earlier dump is gone and the
new one is not kept either: no half-written `Name.*` files are left behind. The
dialog says so when there is an earlier dump. The free-space figure in the
dialog includes the space the earlier dump frees.

In `ALL`, a partition that cannot be opened (step 1) does not stop the run. The
first failure after that point does, so that a full or failing card does not
cost every following partition its earlier dump.

## Files larger than 4 GiB

libnx 4.12.0 does not split a large file, and does not stop you from making one
that is too large. The app has to split, and this one does.

What I checked, in the libnx source at tag v4.12.0:

- `fsFsCreateFile`, `fsFileWrite`, `fsFileSetSize` and `fsStorageRead` pass the
  64-bit size and offset to the FS sysmodule unchanged.
- A search of the source tree found no code that splits a file, reacts to a
  size limit, or looks at which filesystem the card has.
- The stdio layer (`fs_dev.c`) is 64-bit clean as well. When the sysmodule
  refuses a write it returns `-1` with an errno.
- libnx has two opt-in calls for the "concatenation files" of Horizon, the
  Switch's operating system. That is the format for files larger than FAT32
  allows. libnx uses neither on its own:
  `FsCreateOption_BigFile` and `fsFsSetConcatenationFileAttribute`. Such a file
  is a directory with the archive bit that holds numbered parts, which the
  system presents as one file.

So whether one file may exceed 4 GiB is decided by the SD card's filesystem.
exFAT stores 64-bit lengths. FAT32 stores a 32-bit one, so a single file there
cannot be larger than 4 GiB minus one byte.

What I did not establish:

- What the FS sysmodule does when asked for more than that on FAT32: a clean
  error, or a length that wraps. In the Switch 1 FS sysmodule (firmware 23.0.1) I found
  the FAT file write routine, no dedicated "file too large" result, and no
  explicit 4 GiB check. That is a negative result from reading a binary, and
  reading can miss things.
- Anything about the Switch 2 FS sysmodule. Its binary was not available: the
  Switch 2 firmware in `../switch2/` lacks `fs`.

Because of that, the tool does not depend on the sysmodule failing politely.
Dumps over `0xFFFF8000` bytes are written as `Name.bin.00`, `Name.bin.01`, ...
The suffixes have two digits, so the parts sort correctly. A part is 32 KiB
short of 4 GiB, so it is a whole number of clusters on a card with 32 KiB
clusters.

This tool writes plain files and does not use concatenation files. Plain parts
can be copied and hashed on a PC as they are. I have not verified how the
concatenation attribute behaves on a Switch 2.

## Partial dumps

`ZL` dumps the selected partition from its first byte up to an end you choose.

| In the dialog | Moves the end by |
|---|---|
| Left / Right | a 32nd of the partition, 32 MiB at most |
| L / R | a quarter of the partition, 1 GiB at most |
| A | Start. |
| B | Go back. |

Hold a key to repeat. The first move is immediate, the next comes after 400 ms, and then
one every 110 ms.

A partial dump is written as `Name+0x<start>-0x<end>.bin`, for example
`UserDataRoot+0x0-0x40000000.bin`, with both numbers in hex. `<end>` is the first
byte that is not dumped. The dump is split into `.00`, `.01`, ... when splitting (`X`)
is on, with a `.sha256` and an `.info` file of the same name. It only ever
replaces files with exactly that name, so it cannot delete a full dump of the
partition or a partial one with another end. Its `.info` file has the lines of a
full dump's, plus `start` and `source_size`, the size of the whole partition.
Its `size` is the length dumped.

**Where the end starts.** `UserDataRoot` (20) is the whole user area of the NAND.
The partitions inside it, `USER` for one, are what `OpenBisStorage` gives under
their own numbers; `USER` is the one libnx calls `User` (30). If `UserDataRoot`
starts with a partition table that has a `USER` entry, the end starts at the
first byte of `USER`. Otherwise it starts at the full size of the partition, and
you move it down yourself. `L` shows what lies before `USER`: the entries of
the partition table up to it.

**The partition table.** `L` shows the GUID partition table at the start of the
selected partition, with the start and size of every entry, and saves it as
`Name.gpt.txt`. It reads the first 64 KiB and works with a disk block size of
512 or 4096 bytes. When it finds no table, that goes into the log and
`Name.gpt.txt` is not written.

According to switchbrew, on a Switch 1 the user data area of the eMMC is
partition 20, `UserDataRoot`, and starts with that table in plain text. That has
not been confirmed on a console. On a Switch 2 it is not so. The switchbrew page
on the Switch 2's storage
([switchbrew](https://switchbrew.org/wiki/Switch_2:_Flash_Filesystem)) reports
that all of `UserDataRoot` is encrypted, GPT included. That matches what `L`
showed on a Switch 2, which was no table. The page gives no offsets or sizes for
the Switch 2 partitions, and I have none from elsewhere. So on a Switch 2 there
is no known end to move to. It has to be chosen in the dialog. One boundary is
known, from one console's dump: `USER` starts at byte 0x160000000 there, where
a 4 KiB header stands of the same kind as the one at byte 0. The 5.5 GiB
before it are a separate region, most likely `SYSTEM` (see `../FINDINGS.md`).

## The 4 GiB test

`R` runs an experiment on the SD card: it creates an empty file of exactly
4 GiB, opens it again and reads the stored length, then deletes it. It needs
4 GiB free and writes no data. Three outcomes:

| Line in the log | Meaning |
|---|---|
| `CreateFile refused` | The request was refused. Either the card is FAT32, or it has less than 4 GiB free. Make 4 GiB free and run the test again to tell which. |
| `created and read back with the right length` | The card takes files over 4 GiB (exFAT). Single-file dumps work on it, so splitting can be turned off with `X`. |
| a line starting `the length reads back as` | The card accepted the request and did not store the length. Splitting is required, and the filesystem should not be trusted near the limit. |

## Over the network

The network reader is the app's side of this: with it on (`ZR`), a PC can pull
partition bytes from the console over TCP, port 28900. The console is the
server. It only reads NAND, and a transfer writes no dump file on it, only
lines in the log. The PC decides what to ask for, so there is nothing to set on
the console.

Nothing checks who connects. While the reader is on, whatever can reach the
console on its network can read every partition the app may read. `ZR` turns
it off again.

`net_pull.py` in this folder is the client. It needs Python 3 and nothing
else: `python3 net_pull.py ...` works. The examples use `uv`, a tool that runs
Python scripts. `--help` lists the options.

```
uv run --no-project python net_pull.py ping
uv run --no-project python net_pull.py list
uv run --no-project python net_pull.py list --out partitions.txt
uv run --no-project python net_pull.py read 20 0 0x1000000 UserDataRoot-first-16MiB.bin
```

`list` prints one line per partition: its number, `yes` or `no` for whether it
opens, its size in bytes, the result code if it was refused, and its name.

`10.69.68.225` is the address of the project's Switch 2 and the default of
`net_pull.py`. For another console pass its address with `--host`; the app does
not show the console's address. `read` takes the partition number as the list
prints it (`20` is `UserDataRoot`; `30` is `User`, which the Switch 2 refuses),
the first byte, and how many bytes to take. Each may be decimal or hex with
`0x`. `end` or `0` instead of a length means to the end of the partition. The
last argument is the file on the PC to write to. There is no 4 GiB limit on it.
A partition the console refuses comes back as `the console refused: ERR ...`,
with the console's result code.

The console sends a SHA-256 with the bytes of every `read`, and the PC checks
the bytes against it. If the transfer breaks off, the PC asks again from the
bytes it already has, up to five times (`--retries`). If the file is already
there from an earlier run, the read stops with a message, unless `--resume` is
given. Then it continues from where that file ends.

The PC can also have the console hash everything it asked for, without sending
the bytes again, and compare that hash with the one it computes over its own
file. That check costs a second read of the whole range on the console.
`--verify auto` does it after a break or with a resumed file, `--verify yes`
after every read, and `--verify no` never. `<file>.sha256` is written next to
the file, for `sha256sum -c`.

**Checking a dump afterwards.** `verify` has the console hash the range
again, piece by piece, without sending the bytes, and compares every piece
with the file:

```
uv run --no-project python net_pull.py verify 20 0 end switch2nand.bin
```

The pieces are 256 MiB (`--piece` sets another size). They are small on
purpose. The console goes on writing logs and saves to its storage, and what
it wrote since the dump is then one piece that differs, apart from a fault of
the transfer. A hash cannot tell the two apart. So on a piece that does not
match the file's, the piece is read once more and its bytes are compared
with the file's: a few changed blocks are writes of the console's, a run of
megabytes that is missing or shifted is a fault. `--count N` checks only N
pieces spread over the range. To find where the console writes, run it twice
some time apart and compare the hashes: a piece whose hash changes in
between is one the console writes to.

While a PC is connected, the console shows what it asked for. If the PC stops
sending or taking bytes, the console shows for how long, up to two minutes. `B`
asks whether to drop the PC's connection (the screen says "let this reader
go"). `+` exits. After two minutes without a byte the console drops the PC by
itself and listens again, so a connection that was lost does not leave the app
waiting.

The app asks the console not to fall asleep on its own while the app runs. If
the console does not agree, there is a line about it in the log.

Each request is one line of text, sent on a connection of its own, and every
number is decimal. `PING` is answered with `PONG`. `LIST` is answered with one
`PART` line per partition and then `END`. A `PART` line is
`PART <number> <1 or 0> <size> <result> <name>`; the 1 or 0 says whether the
partition opens. `READ <number> <start> <length>` is answered with
`OK <start> <length> <size>`, then the bytes, then `SHA256 <hex>`. `<size>` is
the size of the whole partition. `HASH <number> <start> <length>` is answered
like `READ` without the bytes. A request that is refused gets one `ERR` line
instead. Every line ends with a newline. `nc` is enough to send one:

```
printf 'LIST\n' | nc -N 10.69.68.225 28900
```

The network reader has run on one Switch 2 so far: `PING`, `LIST`, one pull
of all of `UserDataRoot` (255.5 GB in about six hours, at about 11 MiB/s),
and `verify`. Not seen on a console yet: the screen while a PC is silent,
`B` on that screen, and the two-minute limit. What was done with it on a PC
is under [How far this has been checked](#how-far-this-has-been-checked).

## Plaintext or encrypted

`OpenBisStorage` goes through the FS sysmodule, which mounts FAT on top of its
own decryption for the encrypted partitions. A dump of one of those partitions
taken through `OpenBisStorage` is therefore decrypted already. Four taken that
way with this tool on a Switch 2 on 2026-11-23 agree: `DeviceTreeBlob`,
`CalibrationBinary`, `BootPartition1Root` and `BootPartition2Root` are plain
(recorded in `../FINDINGS.md`). It does not hold for `UserDataRoot` (20). On a
Switch 2 all of it is encrypted, GPT included, says switchbrew, and `L` found
no readable table at its start there (see [Partial dumps](#partial-dumps)).
Nothing here decrypts a Switch 2 dump. Its layout can still be read from the zero blocks alone, which are
storage that was never written: `../tools/nand_layout.py` maps a dump by 4 KiB
block and lists the gaps, objects and small records. The offline tool
`../tools/bis_xts_fat.py` is for the Switch 1 dumps in
`../switch1-partitions/`, and the keys of a Switch 2 cannot be dumped at this
time.

To keep a dump made by this tool apart from a raw NAND dump made outside
Horizon, `Name.info` records `looks_plaintext`. It is found by looking at the
first 1 KiB of the dump:

| Value | Meaning |
|---|---|
| `1` | A known signature is there: an `EFI PART` GPT header (id 20), `CAL0` (id 27, `CalibrationBinary`), or a FAT boot sector (ids 28 to 31). |
| `0` | The partition has one of those ids and the signature is not there. The log then has a line about it. |
| `-1` | The partition id has no signature check. |

A raw NAND dump made outside Horizon is XTS-encrypted on a Switch 1, and
`../tools/bis_xts_fat.py` can decrypt one. The Switch 1 dumps in
`../switch1-partitions/` are not settled: they did not decrypt with the repo's
`prod.keys`, and they do not look XTS-encrypted either, since the end of an
object there is zero-padded and `SYSTEM` and `SAFE` begin with a 4 KiB header
holding a 1 KiB run of zeros, not a FAT boot sector (see `../FINDINGS.md`). Do
not run that decryption on a dump from this tool without looking at
`looks_plaintext` first. At `1` the dump is plain already and must not be
decrypted again. At `0` it does not look the way a decrypted dump should, so
find out why before decrypting it. At `-1` the tool has no signature to judge
by, so look at the file yourself.

## Permissions

Opening a BIS partition is allowed or refused per partition, from the caller's
FS access control. The list shows the result code for each partition, so you
can see which ones were refused. For a homebrew process the answer differed
between the two consoles. On a Switch 2, 28 (`CalibrationFile`, `PRODINFOF`),
30 (`User`, `USER`), 31 (`System`, `SYSTEM`) and 33 (`SystemProperPartition`)
answered `0x00000E02 TargetLocked`. On a Switch 1 the same four opened. These
are the partitions the system itself has mounted. The Switch 1 let them be
opened all the same. The Switch 2 refuses them. Why is not settled. What there
is on it is in `../FINDINGS.md`.

At startup, and on `Y`, the tool asks for every partition id from 0 to 255. The
screen says so while it does. If the system were not to answer for some id, the
screen would stay on that line and the console would need a restart.

## How far this has been checked

The engine is `source/bis.c`: scanning, planning, copying and hashing, without
the screen.

| Checked | How |
|---|---|
| The engine never writes to NAND | The only `IStorage` calls in `source/bis.c` are `fsStorageGetSize`, `fsStorageRead` and `fsStorageClose`. |
| libnx and libounce behave the same for this app | The 15 FS and 3 SHA-256 functions it calls are identical in the two libraries, compared function by function. So are the socket files (`socket.c`, `socket.h`), and the call that asks the console not to sleep is the same command in both. |
| Splitting, hashing, cancel, every failure path of the engine, the partition table reader, partial dumps and the requests a PC can send | 956 checks on a PC, with the engine compiled unchanged against a stand-in for libnx's FS calls. |
| What the connection code (`net.c`) does while a PC makes it wait: partial sends, errors, and giving up | 230 checks, with its four socket calls replaced by scripted ones. |
| The UI's flows: list, dialogs, dump, cancel, scroll, `ALL`, no SD card, partition table, partial dumps, key repeat, a PC that is waited for and one that goes silent, the longest partition name | 23 scripted runs with the screen captured and the controls (the pad) scripted. |
| `net_pull.py` against the console's code | Over one real socket on a PC: `ping`, `list`, a transfer that is cut and continued, a whole file checked, a refused read, a PC that sends nothing. |
| The first three suites detect broken code | 85 deliberate bugs, each applied to a copy of `bis.c`, `main.c` or `net.c`. Every one is caught. |

One check is one comparison: a file with the bytes it should hold, a number
with the one expected, or a line of the captured screen with the one looked
for.

Not checked:

- On a console, anything beyond starting and listing (both consoles), `L`
  on `UserDataRoot` (Switch 2), and the network reader on the Switch 2.
  Not observed yet: whether the bytes in a dump written to an SD card come
  out decrypted, and what the 4 GiB test (`R`) gives on a real card.
- The network reader: the screen while a PC is silent, `B` there, and the
  two-minute limit. The console stayed up for the six hours of the pull, but
  which line its log had about sleep was not noted.
- What the real FS sysmodule does at the 4 GiB limit. The stand-in used by the
  tests does not model it.

The stand-in does model two behaviours that the engine's way of writing files
depends on, both taken from the Switch 1 FS sysmodule's write routine:
`CreateFile` fails when the card is full, and a write that would extend a file
is refused (`0x00307202`) unless the file was opened with the `Append` flag.

## Tests

```
./test/run.sh              # the dump engine and the requests a PC can send
./test/transport_test.sh   # the network side while a PC makes it wait
./test/ui_test.sh          # the console UI, scripted
./test/net_test.sh         # net_pull.py against the console's code, over a real socket on one PC
./test/mutation_check.sh   # do the first three fail when the code is broken? (the fourth is not in it)
```

They need a C compiler, `sha256sum`, `timeout` and `nc` (coreutils and OpenBSD
netcat). `net_test.sh` and the mutation check also need `uv`, or `PYTHON=python3`
set. `net_test.sh` listens on port 28943 (`NET_TEST_PORT` changes it). Address
and undefined-behaviour sanitizers are used when the compiler can run them. The
tests write only under `build-host/`. The folders they use as a stand-in SD
card are removed afterwards (`NBE_KEEP_TMP=1` keeps those of `run.sh` and
`ui_test.sh`). The test programs stay in
`build-host/`.

## Layout

| Path | Content |
|---|---|
| `source/bis.c`, `source/bis.h` | The engine. It scans for partitions, plans the output files, copies and hashes, and answers what a PC asks for. No UI. |
| `source/main.c` | The text UI. |
| `source/net.c`, `source/net.h` | The TCP connection to a PC. What the engine sends to a PC and receives from it goes through here. |
| `source/nbe_fmt.c` | Size, result-code and time formatting. |
| `net_pull.py` | The client on the PC. |
| `test/shim/` | The stand-in for the libnx calls the engine uses, with fault injection. |
| `test/ui_shim.c` | The stand-in for console, pad and applet, plus the script and the captured screen. |
| `test/net_stub.c` | The stand-in for the TCP connection in the scripted UI runs. |
| `test/test_bis.c`, `test/test_ui.c`, `test/test_transport.c`, `test/test_net.c` | The engine checks, the UI scenarios, the checks of the network side, and a small server for `net_test.sh`. |
| `test/mutation_check.sh`, `test/mutate.py` | Applies one deliberate bug to a copy of a source file. |
| `build.sh`, `Makefile` | Builds one platform inside the docker image. |
| `test/*.sh` | The suites of the Tests section. What each needs is in its first lines. |
| `build-host/` | Where the tests build and write. |
