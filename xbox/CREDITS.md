# Credits

Uncharted NDC Launcher for Xbox is an experimental UWP port that runs only with the player's own
decrypted dump. It contains no game code or game assets. The full acknowledgement list is in
[../CREDITS.md](../CREDITS.md).

Built from:

- **uncharted_pc** by SuperMedo and contributors (GPL-2.0-or-later) — the compatibility layer this
  UWP host drives.
- [**gow3_xbox**](https://github.com/Cosmo/gow3_xbox) by Cosmo (GPL-2.0) — the proven Xbox UWP host, packaging, guest-memory pager and
  Dozen integration adapted here.
- [**SpartanPort**](https://github.com/FelipePrado0/SpartanPort) — the Xbox compatibility base that the gow3_xbox host derives from.
- **bloodborne_pc** (bbport) by deadinside28 (GPL-2.0) — the origin of the `bbport_*` layer still
  used for settings, overlay, launcher helpers and build scripts.
- **shadPS4** (GPL-2.0) and **shadPS4-Bloodborne-DLSS-FSR** by IFreemz — the renderer, HLE
  libraries, DLSS bridge and Videodec2 work.
- [**KytyPS5**](https://github.com/Cosmo/KytyPS5) — the reference design for the cold-page pager.
- **Mesa / Dozen** — the Vulkan 1.2 driver on D3D12 used on the console.
- **BilalReloaded's Uncharted port**, **drakolordx7's community build**, **Mevlock's dev builds**
  and the upstream contributors — fixes merged into the PC layer.
