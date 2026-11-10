# Dependencies

Nothing is vendored. The stub backend builds with the iOS SDK alone.

## The emulation engine

There is no ready-made iOS engine for this console. The practical base is
**xemu** (`https://github.com/xemu-project/xemu`), added here as a git
submodule and wrapped by a backend that implements
`Core/include/ember/core_api.h`. See `docs/ARCHITECTURE.md` §3 for what the
port involves and why it is the recommended route.

```sh
git submodule add https://github.com/xemu-project/xemu Dependencies/xemu
```

Likely additional pieces:

- **MoltenVK**, to run xemu's Vulkan renderer on Metal.
- **glib** and the other QEMU build dependencies, cross-built for iOS. The
  UTM project maintains build scripts for QEMU's dependency set on iOS and
  is the best reference for this step.

## Licences

- xemu and QEMU are **GPL-2.0**. An app that links them is a derived work:
  the whole app, including this shell, must then be distributed under
  GPL-compatible terms with source available.
- Cxbx-Reloaded (useful as a reference for kernel HLE behaviour) is also GPL.
- MoltenVK is Apache-2.0.

Check every bundled third-party library separately and keep attribution.

## What must never be added

Game images, the MCPX boot ROM, BIOS images, hard-disk images, EEPROM dumps
or keys. They are copyrighted; users supply their own from their own console.
