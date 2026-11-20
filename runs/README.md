# Run journal

One folder per console run, plus the screenshots that run produced. The goal these runs feed is in
[`GOAL.md`](../GOAL.md); the summary table is [`INDEX.md`](INDEX.md).

The rule is the same as the rest of the repository: **console evidence only**. A run folder records
what happened, including the runs that failed, crashed or never reached gameplay. No entry is
deleted because it is inconvenient.

## Layout

```text
runs/
  README.md                  this file
  INDEX.md                   one row per run
  _TEMPLATE.md               the model every run.md starts from
  000-reconstrucao/          baseline reconstructed from the documents (not a run)
    run.md
  status-do-momento-19-11-21h47/   one run
    run.md                   the entry itself
    captures/                screenshots of that run (git-ignored)
```

The folder name is `<state of the moment>-DD-MM-HHmm`, so the order on disk reads as a timeline and
the folder says what was being tested.

## Creating an entry

```powershell
./xbox/scripts/new-run.ps1 -Title 'status do momento' -Mode entry -Switches 'BB_SKIP_COPY=1'
```

That creates `runs/status-do-momento-DD-MM-HHmm/` with `run.md` and `captures/`, and adds a row to
`INDEX.md`. Fill the file in during and after the run.

## What every entry must contain

- Build identity: package version, build stamp from `LocalState/loader.log`, and the commit.
- The exact `run.txt` mode and **every** `BB_*` / `SPARTAN_*` switch that was set.
- Telemetry: duration, presented frames, mean FPS, frame time, current and peak RAM, pipelines
  compiled, and the HUD counters (`FIN`, `ARENA`, `QUAD`).
- Whether the run reached **real gameplay** or only menu/movie/static scene, with the evidence used
  to decide (level name, scripted input, guest log line).
- The first failure: HRESULT, assert text, fault address, scheduler tick.
- Visual symptoms observed, in plain words.
- What the run proves and what the next single action is.

## Screenshots

- Screenshots live in that run's own `captures/` folder and are **git-ignored**: the repository
  ships no game imagery.
- Take one capture per distinct gameplay scene, plus one per defect worth naming.
- Capture with `./xbox/scripts/xbox-screenshot.ps1`, then move the PNG into `captures/`.
- Embed it in `run.md` with a relative link and a caption:

  ```markdown
  ![boat scene: characters present, black triangle on the right](captures/boat-scene.png)
  ```

- If the console screenshot fails, say so in the entry instead of describing from memory.

## Never record

- The console URL, IP address or user name.
- Signing material or credentials.
- Game hashes, dumps or captures inside a tracked file — screenshots stay in the ignored folder.

## How an entry is written

The entry is written from the run's logs and screenshots, not from memory. If a value was not
measured, the field says `not measured`; an empty field is a bug in the entry.
