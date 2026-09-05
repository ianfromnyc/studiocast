# scripts/

This directory contains **developer and setup helpers** for StudioCast.

Top-level scripts are intended to be stable entrypoints. Implementation lives in subdirectories.

## Entry points

- `./scripts/setup.sh` — setup helper (distro dispatcher: Ubuntu-family -> `setup/ubuntu.sh`, Fedora-family (tested on Fedora 44) -> `setup/fedora.sh`)
- `./scripts/install.sh` — installer helper (subcommands)
- `./scripts/installer.sh` — GUI/CLI installer backend entrypoint
- `./scripts/uninstall.sh` — uninstall helper

Common installs:

- GUI/backend installer status + plan:
  - `./scripts/installer.sh status`
  - `./scripts/installer.sh plan install`
- Full install (builds required targets, installs/enables the user service, installs curated Open Audio+Video model packs):
  - `./scripts/install.sh full -y --build-dir ./build`
- List curated model packs without downloading:
  - `./scripts/install.sh full --list`

Legacy script names are kept as compatibility wrappers for now.

## Layout

```text
scripts/
  setup/       # distro + machine setup helpers
  install/     # install helpers (service install, model downloads)
  uninstall/   # uninstall helpers
  dev/         # developer tooling (formatting etc.)
  models/      # model conversion / generation tooling
  _lib/        # shared bash helpers
```

## Model pack templates

Metadata-only model pack templates (no binaries) live under:

```text
resources/model_packs/
```

Installers and docs refer to these templates.
