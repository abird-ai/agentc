#!/usr/bin/env python3
"""gen-exts.py — emit the static extension registry for agentc.

Reads extensions/manifest.json:

    {
      "extensions": [
        {"name": "hello",     "c":    "extensions/hello/hello.c",
         "defines": ["HELLO_VETO=1"]},
        {"name": "rust_echo", "rust": "extensions/rust_echo"}
      ]
    }

and writes build/exts.c with a {name, entry} table plus
`void agentc_exts_register_all(void)`. src/ext/linked.c includes that file when
it exists (the Makefile compiles linked.c), so only linked.c needs to be
recompiled after regenerating: this tool touches it.

Each entry compiles in behind `#ifdef AGENTC_STATIC_EXT_<ident>`: a build that
does not link an extension still compiles, the table entry becomes NULL (see
build/exts.c), and agentc_ext_adopt() logs "not linked" and skips it at startup.
The reference is deliberately not weak: lld's Mach-O linker rejects a weak
import it cannot resolve.

C extensions keep the canonical `agentc_ext_init` export and are compiled with

    clang  -Dagentc_ext_init=agentc_ext_hello_init ...

Rust staticlibs export the per-ident `agentc_ext_<ident>_init` symbol directly
and must not export the canonical `agentc_ext_init` (that would collide when two
staticlibs link one binary), so any number of C and Rust entries build from the
manifest without a Makefile edit.

`--mk-out build/exts.mk` writes the Makefile fragment the extension build
targets consume (the Makefile `-include`s it): the C object list, the per-entry
compile rules (including `defines`), and the Rust staticlib paths and force-link
flags, all derived from this manifest. `--self-test` runs the manifest
validation checks wired into `make check`.
"""
import argparse
import json
import os
import re
import sys
import tempfile
from collections import namedtuple
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

NAME_RE = re.compile(r"[a-z0-9_.:-]+")
# The name must survive sanitize() as a C identifier: a leading digit (`2fast`)
# would otherwise emit `agentc_ext_2fast_init`, which the generated table (and
# the Makefile's -D) cannot compile.
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
DEFINE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_]*)?")
# Paths are interpolated into Makefile recipes, so allow only simple relative
# paths without shell metacharacters.
PATH_RE = re.compile(r"[A-Za-z0-9_./+-]+")

# name ident c rust defines lib
Entry = namedtuple("Entry", "name ident c rust defines lib")


class ManifestError(Exception):
    """A manifest the registry writer would reject."""


def atomic_write_if_changed(path: Path, text: str) -> bool:
    """Write `text` to `path` only when it differs, atomically.

    The temp file is created in the destination directory and renamed with
    os.replace, so a concurrent reader (make parsing the generated fragment, a
    compiler reading the registry) sees either the old or the new complete
    file, never a torn write or a half-written fragment. Returns True when a
    write happened.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        old = path.read_text()
    except OSError:
        old = None
    if old == text:
        return False
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp",
                               dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        os.chmod(tmp, 0o644)   # mkstemp creates 0600; the old write_text made 0644
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return True


def sanitize(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def cargo_lib_name(crate: Path) -> str:
    """The staticlib basename cargo emits for a Rust entry (`lib<name>.a`).

    `[lib] name` wins; otherwise the package name with '-' folded to '_'.
    """
    try:
        text = (crate / "Cargo.toml").read_text(encoding="utf-8")
    except OSError as exc:
        raise ManifestError(f"{crate}/Cargo.toml: {exc}") from exc
    section = ""
    package = None
    lib = None
    name_re = re.compile(r'name\s*=\s*"([^"]+)"')
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        m = name_re.fullmatch(line)
        if not m:
            continue
        if section == "package" and package is None:
            package = m.group(1)
        elif section == "lib" and lib is None:
            lib = m.group(1)
    if lib:
        return lib
    if package:
        return package.replace("-", "_")
    raise ManifestError(f"{crate}/Cargo.toml: no [package] name")


def load_manifest(path: Path):
    """Validate the manifest and return Entry rows.

    Every mode (registry, --list-c, --list-defines, --mk-out) validates through
    here, so a listing cannot paper over a manifest the registry writer would
    refuse.
    """
    if not path.is_file():
        raise ManifestError(f"manifest not found: {path}")
    data = json.loads(path.read_text())
    extensions = data.get("extensions", [])
    if not isinstance(extensions, list):
        raise ManifestError("manifest 'extensions' must be a list")

    entries = []
    seen_names = set()
    seen_idents = {}
    for i, p in enumerate(extensions):
        if not isinstance(p, dict):
            raise ManifestError(f"extension #{i}: not a JSON object")
        name = p.get("name")
        if not isinstance(name, str) or not NAME_RE.fullmatch(name):
            raise ManifestError(f"extension #{i}: invalid or missing name")
        if len(name) > 64:
            # ext_valid_name() refuses at runtime; enforce the same bound here so
            # a manifest cannot build an extension the loader will silently drop.
            raise ManifestError(
                f"extension {name!r}: name longer than 64 bytes")
        if name in seen_names:
            raise ManifestError(f"duplicate extension name: {name}")
        seen_names.add(name)
        ident = sanitize(name)
        if not IDENT_RE.fullmatch(ident):
            raise ManifestError(
                f"extension {name!r}: sanitized name {ident!r} is not a valid C identifier")
        other = seen_idents.get(ident)
        if other is not None:
            # sanitize('a.b') == sanitize('a_b'); both would emit
            # agentc_ext_a_b_init and collide in the generated table.
            raise ManifestError(
                f"extension names '{other}' and '{name}' both map to identifier '{ident}'")
        seen_idents[ident] = name
        c_path = p.get("c")
        rust_path = p.get("rust")
        if c_path is not None and not isinstance(c_path, str):
            raise ManifestError(f"extension {name}: 'c' must be a string")
        if rust_path is not None and not isinstance(rust_path, str):
            raise ManifestError(f"extension {name}: 'rust' must be a string")
        if bool(c_path) == bool(rust_path):
            raise ManifestError(f"extension {name}: set exactly one of 'c' or 'rust'")
        for kind, rel in (("c", c_path), ("rust", rust_path)):
            if rel and (not PATH_RE.fullmatch(rel) or ".." in Path(rel).parts):
                raise ManifestError(
                    f"extension {name}: {kind} path must be a simple relative path: {rel!r}")
        if c_path and not (ROOT / c_path).is_file():
            raise ManifestError(f"extension {name}: C source not found: {c_path}")
        if rust_path and not (ROOT / rust_path / "Cargo.toml").is_file():
            raise ManifestError(f"extension {name}: Rust crate not found: {rust_path}")
        defines = p.get("defines", [])
        if defines is None:
            defines = []
        if not isinstance(defines, list) or not all(
                isinstance(d, str) and DEFINE_RE.fullmatch(d) for d in defines):
            raise ManifestError(
                f"extension {name}: 'defines' must be a list of NAME[=value] tokens")
        if defines and rust_path:
            raise ManifestError(
                f"extension {name}: 'defines' apply to C entries only")
        lib = cargo_lib_name(ROOT / rust_path) if rust_path else None
        entries.append(Entry(name, ident, c_path, rust_path, tuple(defines), lib))
    # Any number of C and Rust entries link: every entry exports only its
    # per-ident `agentc_ext_<ident>_init` symbol. A Rust crate must not also
    # export the canonical `agentc_ext_init`, which would collide at link time.
    return entries


def c_obj(entry: Entry) -> str:
    return f"build/extensions/{entry.ident}.o"


def rust_lib(entry: Entry) -> str:
    return f"build/cargo/{entry.ident}/release/lib{entry.lib}.a"


def emit_mk(entries, path: Path) -> None:
    """Write the Makefile fragment the extension build targets consume.

    It carries the manifest-derived C objects, Rust staticlibs and force-link
    flags plus one build rule per entry. The Makefile `-include`s it after
    defining CC/TST_CFLAGS/NATIVE_EXTRA.
    """
    c_objs = [c_obj(e) for e in entries if e.c]
    rust_libs = [rust_lib(e) for e in entries if e.rust]
    rust_force = [f"-Wl,-u,agentc_ext_{e.ident}_init" for e in entries if e.rust]
    lines = [
        "# generated by tools/gen-exts.py — do not edit.",
        "# Manifest-derived extension build: C objects, Rust staticlibs and flags.",
        f"EXT_C_OBJS := {' '.join(c_objs)}",
        f"EXT_RUST_LIBS := {' '.join(rust_libs)}",
        f"EXT_RUST_FORCE := {' '.join(rust_force)}",
        "",
    ]
    for e in entries:
        if e.c:
            flags = " ".join(
                [f"-Dagentc_ext_init=agentc_ext_{e.ident}_init"]
                + [f"-D{d}" for d in e.defines])
            lines += [
                f"{c_obj(e)}: {e.c} build/version.h | build",
                "\t@mkdir -p $(dir $@)",
                f"\t@echo \"  CC  {e.c}\"",
                f"\t$(CC) $(TST_CFLAGS) $(NATIVE_EXTRA) {flags} -c -o $@ $<",
                "",
            ]
        if e.rust:
            srcs = (f"$(shell find {e.rust} crates -name '*.rs' "
                    f"-o -name 'Cargo.toml' 2>/dev/null)")
            lines += [
                f"{rust_lib(e)}: {srcs} | build",
                f"\tCARGO_NET_OFFLINE=true cargo build --release "
                f"--manifest-path {e.rust}/Cargo.toml --target-dir build/cargo/{e.ident}",
                "",
            ]
    atomic_write_if_changed(path, "\n".join(lines))


def self_test() -> int:
    """Manifest-validation self-checks (wired into `make check`)."""
    hello = "extensions/hello/hello.c"
    cases = [
        ("valid-c", [{"name": "hello", "c": hello}], True),
        ("valid-defines", [{"name": "hello", "c": hello,
                            "defines": ["HELLO_VETO=1"]}], True),
        ("valid-rust", [{"name": "rust_echo", "rust": "extensions/rust_echo"}], True),
        ("two-rust", [{"name": "rust_echo", "rust": "extensions/rust_echo"},
                      {"name": "rust_echo2", "rust": "extensions/rust_echo"}], True),
        ("leading-digit", [{"name": "2fast", "c": hello}], False),
        ("uppercase-name", [{"name": "Hello", "c": hello}], False),
        ("long-name", [{"name": "a" * 65, "c": hello}], False),
        ("name-at-limit", [{"name": "a" * 64, "c": hello}], True),
        ("ident-collision", [{"name": "a.b", "c": hello},
                             {"name": "a_b", "c": hello}], False),
        ("missing-kind", [{"name": "ghost"}], False),
        ("missing-source", [{"name": "ghost", "c": "extensions/nope/nope.c"}], False),
        ("bad-define", [{"name": "hello", "c": hello,
                         "defines": ["1BAD=1"]}], False),
        ("rust-defines", [{"name": "rust_echo", "rust": "extensions/rust_echo",
                           "defines": ["X=1"]}], False),
    ]
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        for label, ext_list, expect_ok in cases:
            p = Path(td) / f"{label}.json"
            p.write_text(json.dumps({"extensions": ext_list}))
            try:
                entries = load_manifest(p)
                got = True
            except ManifestError as exc:
                entries = []
                got = False
                if expect_ok:
                    print(f"self-test {label}: unexpected error: {exc}",
                          file=sys.stderr)
            if got != expect_ok:
                print(f"self-test {label}: expected "
                      f"{'ok' if expect_ok else 'reject'}, got "
                      f"{'ok' if got else 'reject'}", file=sys.stderr)
                fails += 1
            if got and label == "valid-defines" and entries[0].defines != ("HELLO_VETO=1",):
                print(f"self-test {label}: defines not parsed", file=sys.stderr)
                fails += 1
            if got and label == "valid-rust" and entries[0].lib != "agentc_rust_echo":
                print(f"self-test {label}: lib name {entries[0].lib!r}",
                      file=sys.stderr)
                fails += 1
    if fails:
        return 1
    print(f"gen-exts self-test: {len(cases)} cases ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="emit build/exts.c from extensions/manifest.json")
    ap.add_argument("--manifest", default=str(ROOT / "extensions" / "manifest.json"))
    ap.add_argument("--out", default=str(ROOT / "build" / "exts.c"))
    ap.add_argument("--mk-out", default=None,
                    help="write the Makefile fragment (manifest-derived build rules)")
    ap.add_argument("--no-touch", action="store_true",
                    help="do not touch src/ext/linked.c after writing the registry")
    ap.add_argument("--list-c", action="store_true",
                    help="print '<ident> <c-source>' lines for the manifest's C extensions")
    ap.add_argument("--list-defines", action="store_true",
                    help="print the -D flags for the manifest's extension entries")
    ap.add_argument("--kind", choices=("c", "rust", "all"), default="all",
                    help="with --list-defines: restrict to one implementation kind")
    ap.add_argument("--self-test", action="store_true",
                    help="run the manifest-validation self-checks and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        entries = load_manifest(Path(args.manifest))
    except (ManifestError, json.JSONDecodeError, OSError) as exc:
        print(f"gen-exts: {exc}", file=sys.stderr)
        return 1

    if args.mk_out:
        emit_mk(entries, Path(args.mk_out))
        return 0

    if args.list_c:
        for e in entries:
            if e.c:
                print(e.ident, e.c)
        return 0

    if args.list_defines:
        flags = []
        for e in entries:
            if args.kind == "c" and not e.c:
                continue
            if args.kind == "rust" and not e.rust:
                continue
            flags.append(f"-DAGENTC_STATIC_EXT_{e.ident}")
        print(" ".join(flags))
        return 0

    lines = []
    lines.append("/* generated by tools/gen-exts.py — do not edit.")
    lines.append(" *")
    lines.append(" * Per-extension entry points: a build that links the matching object/staticlib")
    lines.append(" * declares AGENTC_STATIC_EXT_<ident> (make release-exts passes them), otherwise")
    lines.append(" * the table entry is NULL and agentc_ext_adopt() skips the extension with a")
    lines.append(" * debug log. The reference is deliberately not weak: lld's Mach-O linker")
    lines.append(" * rejects a weak import it cannot resolve.")
    lines.append(" *")
    lines.append(" * C extensions:      clang ...  -Dagentc_ext_init=<symbol> ...")
    lines.append(" * Rust extensions:   the staticlib exports <symbol> (see extensions/rust_echo).")
    lines.append(" */")
    lines.append('#include "ext/registry_int.h"')
    lines.append("")
    lines.append("#define AGENTC_EXT_ENTRY 1")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    const char *name;")
    lines.append("    int (*init)(const AgcExtHost *host, AgcExt *out);")
    lines.append("} AgcExtRegEntry;")
    lines.append("")
    if entries:
        for e in entries:
            sym = f"agentc_ext_{e.ident}_init"
            src = e.c if e.c else e.rust
            lines.append(f"#ifdef AGENTC_STATIC_EXT_{e.ident}")
            lines.append(f"extern int {sym}(const AgcExtHost *, AgcExt *);"
                         f"   /* {e.name}: {src} */")
            lines.append("#else")
            lines.append(f"#define {sym} NULL")
            lines.append("#endif")
        lines.append("")
    lines.append("void agentc_exts_register_all(void) {")
    lines.append("    static const AgcExtRegEntry agentc_exts_table[] = {")
    for e in entries:
        lines.append(f'        {{ "{e.name}", agentc_ext_{e.ident}_init }},')
    lines.append("        { NULL, NULL },")
    lines.append("    };")
    lines.append("    for (size_t i = 0; agentc_exts_table[i].name; i++)")
    lines.append("        (void)agentc_ext_adopt(agentc_exts_table[i].name, agentc_exts_table[i].init);")
    lines.append("}")
    lines.append("")

    out = Path(args.out)
    atomic_write_if_changed(out, "\n".join(lines))

    if not args.no_touch:
        linked_c = ROOT / "src" / "ext" / "linked.c"
        if linked_c.is_file():
            linked_c.touch()

    print(f"gen-exts: wrote {out} ({len(entries)} extension(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
