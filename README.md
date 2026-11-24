# cheeky-nands (with the lads)

Switch 2 homebrew to read NAND partitions and dump them to an SD card or over network to a PC.

## Running it

Copy the NRO for your console to the SD card under /switch/ and start it from the homebrew
menu.

### What to dump

On the Switch 2, 0 (BootPartition1Root, LUN0), 10 (BootPartition2Root, LUN1) and UserDataRoot (20) could be used to reconstruct the UFS contents after a brick. UserDataRoot contains the encrypted contents, the rest are all decrypted.

Beware: UserDataRoot is 256GB, and over network it'll take 2-6 hours to dump it, and ~2 more to validate it. You still want to dump it, but maybe leave it overnight. Plug it to power or smth. Make sure you have enough free space beforehand. If dumping to a microsd express, it should be ~10x faster.

[See this Switchbrew article for background](https://switchbrew.org/wiki/Switch_2:_Flash_Filesystem).

Before anyone tries to make a big deal about PRODINFO again: It's all just [calibration data](https://switchbrew.org/wiki/Calibration) with not even any partly-certificates within (on switch 2, they're reportedly on PRODINFOF). This is not the first time someone dumps it, and won't be the last.

### How to dump

- **On the console**: pick a partition and dump it to `sdmc:/cheeky-nands/`.
- **Over the network**: turn on the network reader, then pull partition bytes
  with `net_pull.py` from a PC. You probably want to check it against
  the console afterwards too.

### Pulling over the network

`ZR` turns the console into a TCP server on port 28900.

The included `net_pull.py` can be used with base python3 to interact with it. Here are some example commands:

```
python3 net_pull.py ping --host your.console.ip.here
python3 net_pull.py list --host your.console.ip.here
python3 net_pull.py list --out partitions.txt --host your.console.ip.here
python3 net_pull.py read 0 0 end BootPartition1Root.bin --host your.console.ip.here
python3 net_pull.py read 10 0 end BootPartition2Root.bin --host your.console.ip.here
python3 net_pull.py read 20 0 end UserDataRoot.bin --host your.console.ip.here
```

You can and should also validate a NAND backup like so:

```
python3 net_pull.py verify 20 0 end UserDataRoot.bin
```

This will generate and compare hashes of chunks on both console and PC, and if any parts differ, it'll compare them byte by byte. You should expect some minor changes between dump and validate, as the system is running while we're doing this. If you see major differences, dump again :(
