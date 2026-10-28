#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build separate FP32 PyroWave libraries; publish only after both links succeed."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

DEFAULT_SOURCE = Path(__file__).resolve().parents[1] / (
    "Limelight/Stream/PyroWave/MLPyrowaveMetalShaders.h")
SHADERS = {"idwt_msl_source": "pyrowave-idwt",
           "wavelet_dequant_msl_source": "pyrowave-dequant"}
PREFIX = b"pyrowave-metal3.0-fp32-v1\n"


def extract_sources(header):
    sources, definitions = {}, {}
    opening, closing = 'R"PYROWAVE_MSL(', ')PYROWAVE_MSL"'
    for name in SHADERS:
        matches = list(re.finditer(rf"\b{name}\s*(?:\[[^\]]*\]\s*)?=\s*", header))
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one definition of {name}")
        definitions[name] = matches[0]
    for name, definition in definitions.items():
        start = definition.end()
        limit = min((m.start() for m in definitions.values() if m.start() > definition.start()),
                    default=len(header))
        if not header.startswith(opening, start):
            raise ValueError(f"Missing exact raw-string opener for {name}")
        start += len(opening)
        end = header.find(closing, start, limit)
        if end < 0 or not re.match(r"\s*;", header[end + len(closing):limit]):
            raise ValueError(f"Missing terminator or concatenated source for {name}")
        sources[name] = header[start:end]
    return sources


def signature(source):
    return hashlib.sha256(PREFIX + source.encode("utf-8")).hexdigest()


def build(source, output, work=None):
    sources = extract_sources(Path(source).read_bytes().decode("utf-8"))
    output = Path(output)
    with tempfile.TemporaryDirectory(prefix="pyrowave-metal-", dir=work) as tmp:
        tmp = Path(tmp)
        products = {}
        for name, stem in SHADERS.items():
            msl, air, lib = (tmp / (stem + ext) for ext in (".metal", ".air", ".metallib"))
            msl.write_bytes(sources[name].encode("utf-8"))
            subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-std=metal3.0",
                            "-mmacosx-version-min=13.0", "-fno-fast-math", "-c",
                            str(msl), "-o", str(air)], check=True)
            subprocess.run(["xcrun", "-sdk", "macosx", "metallib", str(air),
                            "-o", str(lib)], check=True)
            products[lib.name] = lib.read_bytes()
            products[stem + ".sha256"] = signature(sources[name]).encode("ascii")
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".pyrowave-stage-", dir=output) as stage:
            for name, data in products.items():
                (Path(stage) / name).write_bytes(data)
            for name in products:
                (Path(stage) / name).replace(output / name)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--work", type=Path, help="parent directory for temporary intermediates")
    args = parser.parse_args(argv)
    build(args.source, args.output, args.work)


if __name__ == "__main__":
    main()
