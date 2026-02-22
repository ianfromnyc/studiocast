# scripts/

This directory contains **developer and setup helpers** for StudioCast.

Top-level scripts are intended to be stable entrypoints. Implementation lives in subdirectories.

## Entry points

- `./scripts/setup.sh` — setup helper (Ubuntu-family distro dispatcher)
- `./scripts/install.sh` — installer helper (subcommands)
- `./scripts/uninstall.sh` — uninstall helper
- `./scripts/format.sh` — formatting helper

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
