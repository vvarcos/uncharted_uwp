# Run index

One row per run folder. Details live in `<folder>/run.md`. The folder name is the state of the
moment plus day, month and hour: `status-do-momento-19-11-21h47`.

Created by [`new-run.ps1`](../xbox/scripts/new-run.ps1); completed by hand after the run.
Newest first.

| Run (folder) | Date, hour | Mode | Gameplay? | Mean FPS | Peak RAM | Device loss | Note |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [patch-aplicado-ainda-trava-20-11-1503](patch-aplicado-ainda-trava-20-11-1503/run.md) | 20-11 1503 | entry-unresolved | no | — | — | device lost | patch applied (`applied hash=f4b18264`), still hangs: loop refuted |
| [patch-de-shader-sem-marcador-20-11-1458](patch-de-shader-sem-marcador-20-11-1458/run.md) | 20-11 1458 | entry-unresolved | no | — | — | device lost | patch experiment blind: no marker in loader.log |
| [laco-de-luz-limitado-por-buffer-20-11-1448](laco-de-luz-limitado-por-buffer-20-11-1448/run.md) | 20-11 1448 | entry-unresolved | no | — | — | device lost | last PS before the hang loops over a light count read from a buffer |
| [shader-do-jogo-copia-e-ainda-trava-20-11-1441](shader-do-jogo-copia-e-ainda-trava-20-11-1441/run.md) | 20-11 1441 | entry-unresolved | no | — | — | device lost | guest shader copies, no host copy, same hang |
| [gravacao-na-thread-chamadora-tambem-cai-20-11-1437](gravacao-na-thread-chamadora-tambem-cai-20-11-1437/run.md) | 20-11 1437 | entry-unresolved | no | — | — | device lost | not a recording-order race |
| [dump-do-submit-que-trava-20-11-1432](dump-do-submit-que-trava-20-11-1432/run.md) | 20-11 1432 | entry-unresolved | no | — | — | device lost | hung submit ends in draws/dispatches; no shader loop |
| [unidades-bc-refutadas-20-11-1429](unidades-bc-refutadas-20-11-1429/run.md) | 20-11 1429 | entry-unresolved | no | — | — | assert no boot | blocks vs pixels: Dozen wants pixels, blocks give `OUT_OF_HOST_MEMORY` |
| [sync-nao-salva-o-copy-20-11-1418](sync-nao-salva-o-copy-20-11-1418/run.md) | 20-11 1418 | entry-unresolved | no | — | — | device lost | image lifetime refuted; the command faults |
| [sem-espera-no-submit-culpado-20-11-1408](sem-espera-no-submit-culpado-20-11-1408/run.md) | 20-11 1408 | entry-unresolved | no | — | — | device lost | failing submit has no waits; fault is in the resources |
| [qualquer-copia-derruba-20-11-1343](qualquer-copia-derruba-20-11-1343/run.md) | 20-11 1343 | entry-unresolved | no | — | — | **device lost** | any submitted copy kills the device |
| [render-copy-tambem-stall-20-11-1311](render-copy-tambem-stall-20-11-1311/run.md) | 20-11 1311 | entry-unresolved | no | — | — | GPU stall | render backend works; stall is on the use of the copied images |
| [modo-2-tambem-cai-20-11-1248](modo-2-tambem-cai-20-11-1248/run.md) | 20-11 1248 | entry-unresolved | no | 13.6 (coin) | — | app exited 18 s | same-format copies also fail |
| [copias-sem-staging-tambem-cai-20-11-1244](copias-sem-staging-tambem-cai-20-11-1244/run.md) | 20-11 1244 | entry-unresolved | no | 13.6 (coin) | 3 821 MiB | app exited 18 s | staging not the blocker |
| [block-row-always-on-fecha-janela-20-11-1232](block-row-always-on-fecha-janela-20-11-1232/run.md) | 20-11 1232 | entry-unresolved | parcial (749 frames) | — | — | assert + sem janela | mudança agora opt-in; revertida em 1530 |
| [continue-com-copias-moeda-e-saida-20-11-1209](continue-com-copias-moeda-e-saida-20-11-1209/run.md) | 20-11 1209 | entry-unresolved | no | 13.1 (coin) | 3 698 MiB | app exited 18 s | copies on: coin whole, load dies |
| [composite-pulado-tela-preta-20-11-0156](composite-pulado-tela-preta-20-11-0156/run.md) | 20-11 0156 | entry-unresolved | no | 57 (black) | 3 570 MiB | no | skipping the composite = black screen |
| [copias-ligadas-gpu-stall-20-11-0152](copias-ligadas-gpu-stall-20-11-0152/run.md) | 20-11 0152 | entry-unresolved | no | 45.6 (menu) | 3 441 MiB | **GPU stall** | copies on: AV gone, stall back at submit 25632 |
| [fin-removido-3-fps-20-11-0139](fin-removido-3-fps-20-11-0139/run.md) | 20-11 0139 | entry-unresolved | **yes** | 3.4 | 4 467 MiB | no | Finish() removed: 511 ms -> 0 ms, 0.5 s -> 292 ms per frame |
| [gameplay-sem-crash-20-11-0133](gameplay-sem-crash-20-11-0133/run.md) | 20-11 0133 | entry-unresolved | **yes** | 0–2.2 | 4 623 MiB | no | level load survived the pin fix; `FIN 62 / 511 ms` per frame |
| [pin-de-arena-no-carregamento-20-11-0121](pin-de-arena-no-carregamento-20-11-0121/run.md) | 20-11 0121 | entry-unresolved | no | — | — | no | void: two packages installed, nothing launched |
| [moeda-corrigida-e-meta-nova-20-11-0108](moeda-corrigida-e-meta-nova-20-11-0108/run.md) | 20-11 0108 | entry-unresolved | no | — | — | no | **coin geometry fixed** (user-confirmed) |
| [indices-pelo-buffer-cache-20-11-0055](indices-pelo-buffer-cache-20-11-0055/run.md) | 20-11 0055 | entry-unresolved | no | — | — | AV 0x10 | cache index read made the coin a thin slice; reverted |
| [moeda-com-renderer-novo-r0-20-11-0046](moeda-com-renderer-novo-r0-20-11-0046/run.md) | 20-11 0046 | entry-unresolved | no (loading) | 43.9 | 3 808 MiB | no | first build with the new DLL; `R 0`, wedge still there |
| [culling-desligado-20-11-0030](culling-desligado-20-11-0030/run.md) | 20-11 0030 | entry-unresolved | no | — | — | no | void: app had already exited |
| [varredura-da-moeda-e-transicao-20-11-0019](varredura-da-moeda-e-transicao-20-11-0019/run.md) | 20-11 0019 | entry-unresolved | no (cutscene) | 14–54 | 4 178 MiB | no | transition frame shows the giant triangle |
| [navegacao-303-sem-capturar-a-moeda-20-11-0011](navegacao-303-sem-capturar-a-moeda-20-11-0011/run.md) | 20-11 0011 | entry-unresolved | no (cutscene) | 57.1 | 4 421 MiB | no | build 303; coin not captured |
| [continue-com-retire-20-11-0004](continue-com-retire-20-11-0004/run.md) | 20-11 0004 | entry-unresolved | no | 48.8 (menu) | 3 751 MiB | hang | continue hung in a GPU wait |
| [moeda-sem-o-fix-instalado-20-11-0004](moeda-sem-o-fix-instalado-20-11-0004/run.md) | 20-11 0004 | entry-unresolved | no (loading) | 55.5 | 3 942 MiB | no | old package: did not test the fix |
| [preparo-no-console-sem-pc-19-11-2338](preparo-no-console-sem-pc-19-11-2338/run.md) | 19-11 2338 | entry-unresolved | no (cutscene) | 53–58 | 4 040 MiB | no | image prepared on the console; movie renders |
| [ajm-contornado-e-pipeline-19-11-2305](ajm-contornado-e-pipeline-19-11-2305/run.md) | 19-11 2305 | entry-realgnm-draw | no | — | 300 MiB | no | passed the Ajm import; Dozen rejected the pipeline |
| [primeiro-entry-com-draw-19-11-2249](primeiro-entry-com-draw-19-11-2249/run.md) | 19-11 2249 | entry-realgnm-draw | no | — | 317 MiB | no | prepared image missing (`M3 FAILED`) |
| [primeiro-build-depois-da-perda-19-11-2230](primeiro-build-depois-da-perda-19-11-2230/run.md) | 19-11 2230 | none | no | — | 332 MiB | no | recovered tree builds, installs and presents |
| [000-reconstrucao](000-reconstrucao/run.md) | 19-11 (written) | none | menu only | — | 3 837–3 897 MiB | no | baseline reconstructed from the documents |
| [fin-removido-3-fps-20-11-0139](fin-removido-3-fps-20-11-0139/run.md) | 20-11 0139 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [copias-ligadas-gpu-stall-20-11-0152](copias-ligadas-gpu-stall-20-11-0152/run.md) | 20-11 0152 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [composite-pulado-tela-preta-20-11-0156](composite-pulado-tela-preta-20-11-0156/run.md) | 20-11 0156 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [continue-com-copias-moeda-e-saida-20-11-1209](continue-com-copias-moeda-e-saida-20-11-1209/run.md) | 20-11 1209 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [block-row-always-on-fecha-janela-20-11-1232](block-row-always-on-fecha-janela-20-11-1232/run.md) | 20-11 1232 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [copias-sem-staging-tambem-cai-20-11-1244](copias-sem-staging-tambem-cai-20-11-1244/run.md) | 20-11 1244 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [modo-2-tambem-cai-20-11-1248](modo-2-tambem-cai-20-11-1248/run.md) | 20-11 1248 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [render-copy-tambem-stall-20-11-1311](render-copy-tambem-stall-20-11-1311/run.md) | 20-11 1311 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [qualquer-copia-derruba-20-11-1343](qualquer-copia-derruba-20-11-1343/run.md) | 20-11 1343 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [sem-espera-no-submit-culpado-20-11-1408](sem-espera-no-submit-culpado-20-11-1408/run.md) | 20-11 1408 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [sync-nao-salva-o-copy-20-11-1418](sync-nao-salva-o-copy-20-11-1418/run.md) | 20-11 1418 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [unidades-bc-refutadas-20-11-1429](unidades-bc-refutadas-20-11-1429/run.md) | 20-11 1429 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [dump-do-submit-que-trava-20-11-1432](dump-do-submit-que-trava-20-11-1432/run.md) | 20-11 1432 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [gravacao-na-thread-chamadora-tambem-cai-20-11-1437](gravacao-na-thread-chamadora-tambem-cai-20-11-1437/run.md) | 20-11 1437 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [shader-do-jogo-copia-e-ainda-trava-20-11-1441](shader-do-jogo-copia-e-ainda-trava-20-11-1441/run.md) | 20-11 1441 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [laco-de-luz-limitado-por-buffer-20-11-1448](laco-de-luz-limitado-por-buffer-20-11-1448/run.md) | 20-11 1448 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [patch-de-shader-sem-marcador-20-11-1458](patch-de-shader-sem-marcador-20-11-1458/run.md) | 20-11 1458 | entry-unresolved | not run yet | not measured | not measured | not measured | |
| [patch-aplicado-ainda-trava-20-11-1503](patch-aplicado-ainda-trava-20-11-1503/run.md) | 20-11 1503 | entry-unresolved | not run yet | not measured | not measured | not measured | |

