# Third-party notices

- **CPU semantics**: GXRuntime's PPC core mirrors the interpreter behavior of
  [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later). This
  is why GXRuntime is licensed GPL-3.0-or-later.
- **`graphics/aurora/`**: a vendored fork of
  [encounter/aurora](https://github.com/encounter/aurora) (MIT). Its original
  license file and notices are retained in that directory.
- **gxcore**: implements GX semantics with Dolphin's VideoCommon and Software
  renderer as the behavioral reference.
- **Design references**: [N64Recomp](https://github.com/N64Recomp/N64Recomp)
  and N64ModernRuntime informed the runtime/bridge/backend layering.
