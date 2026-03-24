#!/usr/bin/env python3
"""
upload_onnx_to_hf.py

Upload ONNX model artifacts (and related metadata like model.json / LICENSE)
to a Hugging Face *model* repository.

This version adds two things that matter for StudioCast:

1) Safe upload of a whole *tree* (e.g. resources/model_packs/open_video)
   without filename collisions, by preserving relative paths.

2) Optional materialization of Git-LFS pointer *.onnx files (common for
   OpenCV Zoo models downloaded via raw.githubusercontent.com), so you can
   upload the *real model bytes* to HF (useful if upstream disappears).

Important: Hugging Face *collections* are just groupings of repos.
`--repo-id` must be a repo like: "10dallasj/studiocast-open-video".
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

try:
    from huggingface_hub import HfApi, hf_hub_download
except Exception:  # pragma: no cover
    print("ERROR: missing dependency huggingface_hub. Install with:", file=sys.stderr)
    print("  python3 -m pip install --user huggingface_hub", file=sys.stderr)
    raise


DEFAULT_ALLOW_PATTERNS = [
    "*.onnx",
    "*.onnx.gz",
    "model.json",
    "README.md",
    "LICENSE*",
    "*.md",
    "*.txt",
    "*.yaml",
    "*.yml",
]

DEFAULT_DENY_PATTERNS = [
    ".git/*",
    "**/.git/*",
    "**/__pycache__/*",
    "*.pyc",
    "*.tmp",
    "*.swp",
]


@dataclass
class SrcLayout:
    src: Path
    is_file: bool
    root_dir: Path  # directory we will scan and stage from
    pack_name: str  # used for default README title


# ------------------------- utilities -------------------------


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            b = f.read(chunk_size)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _match_any(name: str, patterns: Iterable[str]) -> bool:
    for p in patterns:
        if fnmatch.fnmatch(name, p):
            return True
    return False


def discover_files(
    root: Path,
    allow_patterns: List[str],
    deny_patterns: List[str],
) -> List[Path]:
    """
    Return a list of files under `root` that match allow_patterns and do NOT match deny_patterns.
    Matching is done against the *relative path* (POSIX-like) as well as basename.
    """
    files: List[Path] = []
    for p in root.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        base = p.name

        allowed = _match_any(rel, allow_patterns) or _match_any(base, allow_patterns)
        if not allowed:
            continue

        denied = _match_any(rel, deny_patterns) or _match_any(base, deny_patterns)
        if denied:
            continue

        files.append(p)
    files.sort()
    return files


def load_model_json(pack_dir: Path) -> Optional[dict]:
    mj = pack_dir / "model.json"
    if not mj.exists():
        return None
    try:
        return json.loads(mj.read_text(encoding="utf-8"))
    except Exception:
        return None


def build_default_readme(
    pack_dir: Path,
    model_json: Optional[dict],
    staged_rel_paths: List[str],
) -> str:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    title = None
    upstream_repo = None
    upstream_tag = None
    license_name = None

    if model_json:
        # Best-effort extraction (schema may vary)
        title = model_json.get("name") or model_json.get("title")
        origin = model_json.get("origin") or {}
        upstream_repo = origin.get("upstream_repo")
        upstream_tag = origin.get("upstream_tag")
        license_name = model_json.get("license") or origin.get("license")

    if not title:
        title = f"ONNX model pack: {pack_dir.name}"

    lines: List[str] = []
    lines.append(f"# {title}")
    lines.append("")
    lines.append("This repository contains ONNX model artifacts exported for use with StudioCast / Open Video effects.")
    lines.append("")
    lines.append("## Provenance")
    if upstream_repo or upstream_tag:
        lines.append(f"- Upstream repo: `{upstream_repo or 'unknown'}`")
        lines.append(f"- Upstream tag/version: `{upstream_tag or 'unknown'}`")
    else:
        lines.append("- Upstream repo/tag: (not specified in model.json)")
    if license_name:
        lines.append(f"- License: `{license_name}` (see included LICENSE file(s))")
    else:
        lines.append("- License: see included LICENSE file(s)")
    lines.append(f"- Built/uploaded: `{now}`")
    lines.append("")
    lines.append("## Files")
    for rp in staged_rel_paths:
        lines.append(f"- `{rp}`")
    lines.append("")
    lines.append("## Notes")
    lines.append("- You are responsible for complying with upstream license terms when redistributing model weights.")
    lines.append("- These files are provided as-is.")
    lines.append("")
    return "\n".join(lines)


def infer_layout(src: Path) -> SrcLayout:
    src = src.expanduser().resolve()
    if not src.exists():
        raise FileNotFoundError(f"--src does not exist: {src}")

    if src.is_file():
        return SrcLayout(
            src=src,
            is_file=True,
            root_dir=src.parent,
            pack_name=src.stem,
        )

    # directory
    mj = src / "model.json"
    pack_name = src.name
    if mj.exists():
        mj_data = load_model_json(src)
        if mj_data and isinstance(mj_data, dict):
            pack_name = (mj_data.get("name") or mj_data.get("title") or src.name)
    return SrcLayout(
        src=src,
        is_file=False,
        root_dir=src,
        pack_name=pack_name,
    )


# ------------------------- Git LFS pointer handling -------------------------


_LFS_OID_RE = re.compile(r"oid\s+sha256:([0-9a-f]{64})")
_LFS_SIZE_RE = re.compile(r"size\s+(\d+)")


@dataclass
class LfsPointer:
    oid_sha256: str
    size: int


def parse_git_lfs_pointer(path: Path) -> Optional[LfsPointer]:
    """
    Return LfsPointer if `path` looks like a Git LFS pointer file, else None.
    """
    try:
        if path.stat().st_size > 2048:
            return None
        txt = path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return None

    if "git-lfs.github.com/spec/v1" not in txt:
        return None
    if not txt.lstrip().startswith("version "):
        return None

    mo = _LFS_OID_RE.search(txt)
    ms = _LFS_SIZE_RE.search(txt)
    if not mo or not ms:
        return None
    try:
        return LfsPointer(oid_sha256=mo.group(1), size=int(ms.group(1)))
    except Exception:
        return None


def _download_url_to_file(url: str, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(
        url,
        headers={
            # GitHub sometimes requires a UA.
            "User-Agent": "studiocast-model-uploader/1.0",
        },
        method="GET",
    )
    with urllib.request.urlopen(req) as r:  # nosec - user-controlled URL not exposed; internal tooling
        out_path.write_bytes(r.read())


def _verify_download_matches_pointer(tmp_file: Path, ptr: LfsPointer) -> None:
    actual_size = tmp_file.stat().st_size
    if actual_size != ptr.size:
        raise RuntimeError(f"download size mismatch: expected {ptr.size}, got {actual_size}")
    actual_sha = sha256_file(tmp_file)
    if actual_sha.lower() != ptr.oid_sha256.lower():
        raise RuntimeError(f"download sha256 mismatch: expected {ptr.oid_sha256}, got {actual_sha}")


def resolve_open_video_face_detection_yunet(
    pack_dir: Path,
    model_json: Dict,
    ptr: LfsPointer,
    cache_dir: Path,
    prefer: str,
) -> Optional[Path]:
    """
    Resolve YuNet face_detection models from OpenCV Zoo (Git LFS) into a real ONNX file.

    We keep local filename `model.onnx`, but fetch one of:
      - face_detection_yunet_2023mar.onnx
      - face_detection_yunet_2023mar_int8.onnx
      - face_detection_yunet_2023mar_int8bq.onnx

    prefer:
      - "github": use media.githubusercontent.com
      - "hf":     download from opencv/face_detection_yunet on Hugging Face
      - "auto":   try github then hf
    """
    pack_id = str(model_json.get("id") or "")
    if "int8bq" in pack_id:
        upstream_name = "face_detection_yunet_2023mar_int8bq.onnx"
    elif "int8" in pack_id:
        upstream_name = "face_detection_yunet_2023mar_int8.onnx"
    else:
        upstream_name = "face_detection_yunet_2023mar.onnx"

    # We download into a deterministic cache path so repeated runs don't re-download.
    dest = cache_dir / "opencv_zoo" / "face_detection_yunet" / upstream_name
    dest.parent.mkdir(parents=True, exist_ok=True)

    def try_github() -> Optional[Path]:
        # NOTE: raw.githubusercontent.com returns the pointer; media.githubusercontent.com returns real bytes.
        url = f"https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/face_detection_yunet/{upstream_name}"
        tmp = dest.with_suffix(dest.suffix + ".tmp")
        try:
            _download_url_to_file(url, tmp)
            _verify_download_matches_pointer(tmp, ptr)
            tmp.replace(dest)
            return dest
        except Exception as e:
            try:
                if tmp.exists():
                    tmp.unlink()
            except Exception:
                pass
            return None

    def try_hf() -> Optional[Path]:
        try:
            # Public repo; token not required for download.
            hf_path = hf_hub_download(
                repo_id="opencv/face_detection_yunet",
                filename=upstream_name,
                revision="main",
            )
            src = Path(hf_path)
            # Verify against pointer, then copy into our cache.
            _verify_download_matches_pointer(src, ptr)
            shutil.copy2(src, dest)
            return dest
        except Exception:
            return None

    if dest.exists():
        # Validate cached file matches pointer (guards against stale cache).
        try:
            _verify_download_matches_pointer(dest, ptr)
            return dest
        except Exception:
            try:
                dest.unlink()
            except Exception:
                pass

    order = prefer.lower().strip()
    if order not in ("auto", "github", "hf"):
        order = "auto"

    if order in ("auto", "github"):
        p = try_github()
        if p:
            return p
        if order == "github":
            return None

    # auto fallback OR hf-only
    return try_hf()


def try_materialize_lfs_onnx(
    file_path: Path,
    rel_posix: str,
    cache_dir: Path,
    prefer_source: str,
) -> Optional[Path]:
    """
    Attempt to replace a Git-LFS pointer ONNX with real bytes.

    Currently implemented:
      - OpenCV Zoo YuNet face_detection models (fp32 / int8 / int8bq)

    Returns a path to a *real* ONNX file (cached) if resolved, else None.
    """
    ptr = parse_git_lfs_pointer(file_path)
    if not ptr:
        return None

    # Only handle the known StudioCast case for now:
    #   resources/model_packs/open_video/face_detection/*/model.onnx
    parts = Path(rel_posix).parts
    if len(parts) < 3:
        return None
    if parts[0] != "face_detection":
        return None
    if parts[-1] != "model.onnx":
        return None

    pack_dir = file_path.parent
    mj = load_model_json(pack_dir)
    if not mj or not isinstance(mj, dict):
        return None

    return resolve_open_video_face_detection_yunet(
        pack_dir=pack_dir,
        model_json=mj,
        ptr=ptr,
        cache_dir=cache_dir,
        prefer=prefer_source,
    )


# ------------------------- staging + upload -------------------------


def stage_files(
    layout: SrcLayout,
    out_dir: Path,
    allow_patterns: List[str],
    deny_patterns: List[str],
    generate_readme: bool,
    overwrite_readme: bool,
    skip_empty_onnx: bool,
    resolve_lfs_pointers: bool,
    lfs_prefer_source: str,
) -> Tuple[Path, List[str], List[Tuple[str, str]], List[Tuple[str, str]]]:
    """
    Copy selected files into a staging directory, preserving relative paths.

    Returns:
      (staging_root,
       staged_rel_paths,
       skipped_onnx: [(rel, reason)],
       resolved_onnx: [(rel, how)])
    """
    staging_root = out_dir / "staging"
    staging_root.mkdir(parents=True, exist_ok=True)

    cache_dir = out_dir / "cache"
    cache_dir.mkdir(parents=True, exist_ok=True)

    skipped_onnx: List[Tuple[str, str]] = []
    resolved_onnx: List[Tuple[str, str]] = []

    def should_skip_non_onnx(p: Path, rel_posix: str) -> Optional[str]:
        # Only reason to skip is deny-pattern or allow-pattern; those are handled at discovery time.
        # For non-onnx we don't do extra filtering.
        return None

    def handle_onnx_copy(src: Path, rel_posix: str, dest: Path) -> Optional[str]:
        lp = rel_posix.lower()
        if not (lp.endswith(".onnx") or lp.endswith(".onnx.gz")):
            return "not onnx"

        # Skip empty placeholder
        try:
            if skip_empty_onnx and src.stat().st_size == 0:
                return "empty file"
        except Exception:
            pass

        # Resolve Git-LFS pointer if requested
        if lp.endswith(".onnx"):
            ptr = parse_git_lfs_pointer(src)
            if ptr:
                if not resolve_lfs_pointers:
                    return "git-lfs pointer (not real model bytes)"
                resolved = try_materialize_lfs_onnx(
                    file_path=src,
                    rel_posix=rel_posix,
                    cache_dir=cache_dir,
                    prefer_source=lfs_prefer_source,
                )
                if not resolved:
                    return "git-lfs pointer (unresolved; install git-lfs or enable resolver for this pack)"
                shutil.copy2(resolved, dest)
                resolved_onnx.append((rel_posix, f"resolved from {lfs_prefer_source}"))
                return None

        # Normal copy
        shutil.copy2(src, dest)
        return None

    if layout.is_file:
        rel = layout.src.name
        dest = staging_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)

        if rel.lower().endswith(".onnx") or rel.lower().endswith(".onnx.gz"):
            reason = handle_onnx_copy(layout.src, rel, dest)
            if reason:
                skipped_onnx.append((rel, reason))
                staged_rel_paths: List[str] = []
            else:
                staged_rel_paths = [rel]
        else:
            shutil.copy2(layout.src, dest)
            staged_rel_paths = [rel]
    else:
        files = discover_files(layout.root_dir, allow_patterns, deny_patterns)
        staged_rel_paths = []
        for f in files:
            rel = f.relative_to(layout.root_dir).as_posix()
            dest = staging_root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)

            if rel.lower().endswith(".onnx") or rel.lower().endswith(".onnx.gz"):
                reason = handle_onnx_copy(f, rel, dest)
                if reason:
                    skipped_onnx.append((rel, reason))
                    # Clean up any partially copied file
                    try:
                        if dest.exists():
                            dest.unlink()
                    except Exception:
                        pass
                    continue
            else:
                reason = should_skip_non_onnx(f, rel)
                if reason:
                    continue
                shutil.copy2(f, dest)

            staged_rel_paths.append(rel)

    # README generation
    readme_path = staging_root / "README.md"
    if generate_readme:
        should_write = overwrite_readme or (not readme_path.exists())
        if should_write:
            model_json = None
            if not layout.is_file:
                model_json = load_model_json(layout.root_dir)
            content = build_default_readme(
                pack_dir=layout.root_dir if not layout.is_file else layout.src.parent,
                model_json=model_json,
                staged_rel_paths=sorted(staged_rel_paths),
            )
            readme_path.write_text(content, encoding="utf-8")
            if "README.md" not in staged_rel_paths:
                staged_rel_paths.append("README.md")

    staged_rel_paths = sorted(set(staged_rel_paths))
    return staging_root, staged_rel_paths, skipped_onnx, resolved_onnx


def validate_onnx_files(staging_root: Path, rel_paths: List[str]) -> None:
    """
    Best-effort ONNX validation. If onnx isn't installed, we skip with a warning.
    """
    onnx_paths = [staging_root / rp for rp in rel_paths if rp.lower().endswith(".onnx")]
    if not onnx_paths:
        return

    try:
        import onnx  # type: ignore
        from onnx import checker  # type: ignore
    except Exception:
        print("WARN: onnx python package not installed; skipping ONNX validation.", file=sys.stderr)
        print("      Install with: python3 -m pip install --user onnx", file=sys.stderr)
        return

    for p in onnx_paths:
        try:
            m = onnx.load(str(p))
            checker.check_model(m)
        except Exception as e:
            raise RuntimeError(f"ONNX validation failed for {p}: {e}") from e


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Upload ONNX artifacts to Hugging Face Hub (public by default)."
    )
    ap.add_argument(
        "--src",
        required=True,
        help="Path to an ONNX file or a directory tree to upload (e.g. resources/model_packs/open_video).",
    )
    ap.add_argument(
        "--repo-id",
        required=True,
        help="Hugging Face *repo* id, e.g. '10dallasj/studiocast-open-video'. (NOT a collection URL.)",
    )
    ap.add_argument(
        "--token",
        default="",
        help="Hugging Face token. If empty, uses HF_TOKEN / HUGGINGFACE_HUB_TOKEN env vars.",
    )
    ap.add_argument(
        "--private",
        action="store_true",
        help="Create/upload to a private repo (default: public).",
    )
    ap.add_argument(
        "--path-in-repo",
        default="",
        help="Optional subdirectory inside the HF repo to place files.",
    )
    ap.add_argument(
        "--commit-message",
        default="Upload StudioCast ONNX artifacts",
        help="Commit message for the upload.",
    )
    ap.add_argument(
        "--allow",
        action="append",
        default=[],
        help="Allow pattern (glob). Can be specified multiple times. "
             "Default includes *.onnx, model.json, LICENSE*, README.md, etc.",
    )
    ap.add_argument(
        "--deny",
        action="append",
        default=[],
        help="Deny pattern (glob). Can be specified multiple times.",
    )
    ap.add_argument(
        "--no-generate-readme",
        action="store_true",
        help="Do not generate a README.md if missing.",
    )
    ap.add_argument(
        "--overwrite-readme",
        action="store_true",
        help="If set, overwrites README.md even if it exists.",
    )
    ap.add_argument(
        "--validate-onnx",
        action="store_true",
        help="Validate ONNX files with onnx.checker before upload (requires 'onnx' package).",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Prepare staging area and print what would be uploaded, but do not upload.",
    )
    ap.add_argument(
        "--skip-empty-onnx",
        action="store_true",
        help="Skip empty .onnx placeholders (recommended).",
    )
    ap.add_argument(
        "--resolve-lfs-pointers",
        action="store_true",
        help="If set, attempt to download real bytes for known Git-LFS pointer .onnx files (e.g., OpenCV Zoo YuNet).",
    )
    ap.add_argument(
        "--lfs-prefer-source",
        default="auto",
        choices=["auto", "github", "hf"],
        help="Where to fetch real bytes when resolving Git-LFS pointers: auto=try GitHub media first then HF, or force one.",
    )
    ap.add_argument(
        "--fail-on-skipped-onnx",
        action="store_true",
        help="Exit non-zero if any .onnx files were skipped (empty or unresolved pointer).",
    )

    args = ap.parse_args()

    token = args.token.strip() or os.environ.get("HF_TOKEN") or os.environ.get("HUGGINGFACE_HUB_TOKEN") or ""
    if not token and not args.dry_run:
        print("ERROR: No Hugging Face token provided.", file=sys.stderr)
        print("Set HF_TOKEN (recommended) or pass --token.", file=sys.stderr)
        return 2

    allow_patterns = args.allow if args.allow else list(DEFAULT_ALLOW_PATTERNS)
    deny_patterns = args.deny if args.deny else list(DEFAULT_DENY_PATTERNS)

    layout = infer_layout(Path(args.src))

    with tempfile.TemporaryDirectory(prefix="hf_upload_stage_") as td:
        td_path = Path(td)

        staging_root, staged_rel_paths, skipped_onnx, resolved_onnx = stage_files(
            layout=layout,
            out_dir=td_path,
            allow_patterns=allow_patterns,
            deny_patterns=deny_patterns,
            generate_readme=not args.no_generate_readme,
            overwrite_readme=args.overwrite_readme,
            skip_empty_onnx=bool(args.skip_empty_onnx),
            resolve_lfs_pointers=bool(args.resolve_lfs_pointers),
            lfs_prefer_source=str(args.lfs_prefer_source),
        )

        # Optional ONNX validation
        if args.validate_onnx:
            validate_onnx_files(staging_root, staged_rel_paths)

        # Compute SHA256s (nice to have; printed for visibility)
        sha_lines = []
        for rp in staged_rel_paths:
            if rp.lower().endswith(".onnx"):
                p = staging_root / rp
                sha_lines.append(f"{rp}: {sha256_file(p)}")

        print("=== Staged files ===")
        for rp in staged_rel_paths:
            p = staging_root / rp
            size_mb = p.stat().st_size / (1024 * 1024)
            print(f"- {rp}  ({size_mb:.2f} MiB)")

        if resolved_onnx:
            print("")
            print("=== Resolved Git-LFS pointer ONNX files ===")
            for rel, how in resolved_onnx:
                print(f"- {rel}: {how}")

        if skipped_onnx:
            print("")
            print("=== Skipped ONNX files ===")
            for rel, reason in skipped_onnx:
                print(f"- {rel}: {reason}")

        if sha_lines:
            print("")
            print("=== ONNX SHA256 ===")
            for s in sha_lines:
                print(s)

        if skipped_onnx and args.fail_on_skipped_onnx:
            print("")
            print("ERROR: Some ONNX files were skipped and --fail-on-skipped-onnx was set.", file=sys.stderr)
            return 3

        if args.dry_run:
            print("")
            print("DRY RUN: not uploading.")
            print(f"Would upload to: https://huggingface.co/{args.repo_id}")
            return 0

        api = HfApi(token=token)

        # Create repo (public by default unless --private is set)
        api.create_repo(
            repo_id=args.repo_id,
            repo_type="model",
            private=bool(args.private),
            exist_ok=True,
        )

        # Upload as one commit
        api.upload_folder(
            folder_path=str(staging_root),
            repo_id=args.repo_id,
            repo_type="model",
            path_in_repo=args.path_in_repo or "",
            commit_message=args.commit_message,
        )

        print("")
        print("✅ Upload complete:")
        print(f"https://huggingface.co/{args.repo_id}")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
