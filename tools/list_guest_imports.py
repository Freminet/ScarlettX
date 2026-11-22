#!/usr/bin/env python3
"""List the imports of a guest PE image without running it.

The import table of a title is the only public, first-hand evidence of which
xgameruntime entry points it expects: the names are written by the toolchain
that built the title, so they are not guesses. This tool reads that table from
a PE32/PE32+ file and prints the symbols grouped by DLL, so we can tell which
HLE functions a title really uses (and which ones only exist in our headers).

Usage:
    python tools/list_guest_imports.py <image.exe|image.dll>
    python tools/list_guest_imports.py <image> --dll xgameruntime --json
    python tools/list_guest_imports.py <image> --missing-from include/scarlettx/gdk/xgameruntime_abi.h
"""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys

# Data directory index 1 is the import table in both PE32 and PE32+.
_IMPORT_DIRECTORY_INDEX = 1
_DESCRIPTOR_SIZE = 20  # IMAGE_IMPORT_DESCRIPTOR: 5 DWORDs
_IMPORT_BY_NAME_HINT_SIZE = 2


class PeError(Exception):
    """The file is not a PE image this tool understands."""


class Section:
    __slots__ = ("name", "virtual_address", "virtual_size", "raw_pointer", "raw_size")

    def __init__(self, name: str, virtual_address: int, virtual_size: int,
                 raw_pointer: int, raw_size: int) -> None:
        self.name = name
        self.virtual_address = virtual_address
        self.virtual_size = virtual_size
        self.raw_pointer = raw_pointer
        self.raw_size = raw_size

    def contains(self, rva: int) -> bool:
        size = self.virtual_size or self.raw_size
        return self.virtual_address <= rva < self.virtual_address + size


class PeImage:
    """Just enough of the PE format to walk the import directory."""

    def __init__(self, path: pathlib.Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        self._parse_headers()

    # -- headers ---------------------------------------------------------

    def _u16(self, offset: int) -> int:
        if offset + 2 > len(self.data):
            raise PeError(f"truncated file: 2 bytes at offset 0x{offset:X}")
        return struct.unpack_from("<H", self.data, offset)[0]

    def _u32(self, offset: int) -> int:
        if offset + 4 > len(self.data):
            raise PeError(f"truncated file: 4 bytes at offset 0x{offset:X}")
        return struct.unpack_from("<I", self.data, offset)[0]

    def _u64(self, offset: int) -> int:
        if offset + 8 > len(self.data):
            raise PeError(f"truncated file: 8 bytes at offset 0x{offset:X}")
        return struct.unpack_from("<Q", self.data, offset)[0]

    def _u16_at_rva(self, rva: int) -> int:
        return self._u16(self._offset_of(rva))

    def _u32_at_rva(self, rva: int) -> int:
        return self._u32(self._offset_of(rva))

    def _parse_headers(self) -> None:
        if len(self.data) < 0x40 or self.data[:2] != b"MZ":
            raise PeError("missing DOS header")
        pe_offset = self._u32(0x3C)
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PeError("missing PE signature")
        coff = pe_offset + 4
        section_count = self._u16(coff + 2)
        optional_size = self._u16(coff + 16)
        optional = coff + 20
        magic = self._u16(optional)
        if magic == 0x20B:
            self.is_64 = True
            image_base = self._u64(optional + 24)
            directory_base = optional + 112
            self._thunk_size = 8
        elif magic == 0x10B:
            self.is_64 = False
            image_base = self._u32(optional + 28)
            directory_base = optional + 96
            self._thunk_size = 4
        else:
            raise PeError(f"unknown optional header magic 0x{magic:04X}")
        self.image_base = image_base
        directory_count = self._u32(directory_base - 4)
        if directory_count <= _IMPORT_DIRECTORY_INDEX:
            raise PeError("no data directories")
        entry = directory_base + _IMPORT_DIRECTORY_INDEX * 8
        self.import_rva = self._u32(entry)
        self.import_size = self._u32(entry + 4)

        sections_at = optional + optional_size
        self.sections = []
        for index in range(section_count):
            base = sections_at + index * 40
            raw_name = self.data[base:base + 8].split(b"\0", 1)[0]
            self.sections.append(Section(
                raw_name.decode("ascii", "replace"),
                self._u32(base + 12),
                self._u32(base + 8),
                self._u32(base + 20),
                self._u32(base + 16),
            ))
        if not self.sections:
            raise PeError("no sections")

    # -- RVA translation -------------------------------------------------

    def _offset_of(self, rva: int) -> int:
        for section in self.sections:
            if section.contains(rva):
                delta = rva - section.virtual_address
                offset = section.raw_pointer + delta
                if offset >= len(self.data):
                    break
                return offset
        raise PeError(f"RVA 0x{rva:X} is outside the file")

    def _read_c_string_at_rva(self, rva: int) -> str:
        offset = self._offset_of(rva)
        if offset >= len(self.data):
            raise PeError(f"string at RVA 0x{rva:X} is past the end of the file")
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise PeError(f"unterminated string at RVA 0x{rva:X}")
        return self.data[offset:end].decode("ascii", "replace")

    # -- imports ---------------------------------------------------------

    def imports(self) -> dict[str, list[dict]]:
        """Return {dll name: [{"name": str|None, "ordinal": int|None}, ...]}."""
        if not self.import_rva:
            return {}
        result: dict[str, list[dict]] = {}
        cursor = self.import_rva
        while True:
            offset = self._offset_of(cursor)
            original_thunk = self._u32(offset)
            name_rva = self._u32(offset + 12)
            first_thunk = self._u32(offset + 16)
            if not (original_thunk or name_rva or first_thunk):
                break
            dll = self._read_c_string_at_rva(name_rva)
            thunks_rva = original_thunk or first_thunk
            result[dll] = self._thunks(thunks_rva)
            cursor += _DESCRIPTOR_SIZE
        return result

    def _thunks(self, rva: int) -> list[dict]:
        entries: list[dict] = []
        if not rva:
            return entries
        step = self._thunk_size
        for index in range(1 << 16):  # bounded walk, no runaway loops
            offset = self._offset_of(rva + index * step)
            value = self._u64(offset) if self.is_64 else self._u32(offset)
            if value == 0:
                break
            ordinal_flag = 1 << 63 if self.is_64 else 1 << 31
            if value & ordinal_flag:
                entries.append({"name": None, "ordinal": value & 0xFFFF})
            else:
                # IMAGE_IMPORT_BY_NAME: 2-byte hint, then the name.
                entries.append({
                    "name": self._read_c_string_at_rva(value + _IMPORT_BY_NAME_HINT_SIZE),
                    "ordinal": None,
                })
        return entries


def declared_symbols(header: pathlib.Path) -> set[str]:
    """Function names the ABI header declares with its export macro.

    Only macro-marked declarations count: matching every capitalized call would
    treat any helper called in the header as declared, which is exactly how a
    missing import gets hidden.
    """
    import re

    pattern = re.compile(r"SCARLETTX_[A-Z0-9_]*_API\b[^;()]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    names: set[str] = set()
    if header.is_file():
        text = header.read_text(encoding="utf-8", errors="replace")
        names.update(match.group(1) for match in pattern.finditer(text))
    return names


def format_symbol(entry: dict) -> str:
    if entry["name"] is not None:
        return entry["name"]
    return f"#{entry['ordinal']}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=pathlib.Path, help="guest PE file (.exe or .dll)")
    parser.add_argument("--dll", action="append", default=None,
                        help="only show this DLL (case-insensitive, repeatable)")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--missing-from", type=pathlib.Path, default=None,
                        help="header with the HLE declarations; list imports it does not declare")
    args = parser.parse_args(argv)

    try:
        image = PeImage(args.image)
    except (OSError, PeError, struct.error) as exc:
        print(f"[list_guest_imports] {args.image}: {exc}", file=sys.stderr)
        return 1

    try:
        imports = image.imports()
    except (PeError, struct.error) as exc:
        print(f"[list_guest_imports] {args.image}: broken import table ({exc})", file=sys.stderr)
        return 1
    if args.dll:
        wanted = {name.lower() for name in args.dll}

        def selected(dll: str) -> bool:
            lowered = dll.lower()
            stem = lowered[:-4] if lowered.endswith(".dll") else lowered
            for name in wanted:
                # "kernel32" matches KERNEL32.dll; "api-ms-win-core-com" matches
                # its versioned api-ms-win-core-com-l1-1-0.dll.
                if lowered == name or stem == name:
                    return True
                if stem.startswith(name + "-") or lowered.startswith(name + "."):
                    return True
            return False

        imports = {dll: entries for dll, entries in imports.items() if selected(dll)}

    declared: set[str] = set()
    if args.missing_from:
        if not args.missing_from.is_file():
            print(f"[list_guest_imports] no such header: {args.missing_from}", file=sys.stderr)
            return 1
        declared = declared_symbols(args.missing_from)

    report = {
        "image": str(args.image),
        "machine": "x64" if image.is_64 else "x86",
        "image_base": image.image_base,
        "dlls": {
            dll: [format_symbol(entry) for entry in entries]
            for dll, entries in sorted(imports.items(), key=lambda kv: kv[0].lower())
        },
    }
    if args.missing_from:
        report["missing"] = {
            dll: [symbol for symbol in symbols
                  if symbol.startswith("X") and not symbol.startswith("#") and symbol not in declared]
            for dll, symbols in report["dlls"].items()
        }

    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    total = sum(len(entries) for entries in report["dlls"].values())
    print(f"{args.image} ({report['machine']}, base 0x{report['image_base']:X}): "
          f"{len(report['dlls'])} DLLs, {total} imports")
    for dll, symbols in report["dlls"].items():
        print(f"\n{dll} ({len(symbols)})")
        for symbol in symbols:
            marker = ""
            if args.missing_from and symbol in report.get("missing", {}).get(dll, []):
                marker = "   <- not declared"
            print(f"  {symbol}{marker}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
