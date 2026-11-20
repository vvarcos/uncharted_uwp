# Run perfil-sem-console-20-11-1522 — no data: the console left the network

Folder: `runs/perfil-sem-console-20-11-1522/` — no captures.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 15:22 (local) |
| Commit | `287e956` |
| Package | `UnchartedNDCUWP_1.0.26324.1809_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_GPU_PROFILE=1` |

## Intent

The copies are now known to kill the GPU through their **content** (see
`runs/barreiras-sem-copia-jogo-roda-20-11-1515/`). `BB_GPU_PROFILE=1` prints GPU ms per pass every 5 s,
so the last print before the stall would name the pass that explodes when it sees the copied data.

## Result

**Otherwise no result.** The run started, and while it was being watched the console stopped answering:

```text
Portal connection failed. Check reachability, authentication and TLS configuration.
Test-NetConnection 192.168.0.4:11443 → False
ping 192.168.0.4 → 2 sent, 0 received (100% loss)
```

The console has not come back to the network since. The timing (right after the known GPU timeout) makes
a crash/reboot of the console itself the likely cause, but that is not verified: it may equally be the
network.

## Next

When the console answers again:

1. re-run this exact configuration and read the `GPU ...` profile lines before the stall;
2. if the console stays off, the same question can be answered offline from a **dump of every shader of
   the frame** (`BB_DUMP_SHADERS=1`) by scanning for loops whose bound is an `OpLoad` from a buffer —
   the shape that turns "the copy filled real data" into a multi-second submit.
