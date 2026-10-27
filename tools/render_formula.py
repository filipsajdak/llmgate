#!/usr/bin/env python3
import argparse
import re
import sys

REPOSITORY = "filipsajdak/llmgate"
ASSET = re.compile(r"^llmgate-(?P<version>[0-9][^-]*)-(?P<os>darwin|linux)-(?P<arch>arm64|x86_64)\.tar\.gz$")
BLOCKS = (("macos", "darwin"), ("linux", "linux"))
ARCHES = (("arm", "arm64"), ("intel", "x86_64"))


def parse_sums(text):
    found = {}
    for line in text.splitlines():
        digest, _, name = line.strip().partition("  ")
        match = ASSET.match(name)
        if match and re.fullmatch(r"[0-9a-f]{64}", digest):
            found[(match["os"], match["arch"])] = (match["version"], digest)
    return found


def render(sums, tag):
    found = parse_sums(sums)
    versions = {version for version, _ in found.values()}
    if len(versions) != 1 or tag.lstrip("v") not in versions:
        raise ValueError(f"checksums name versions {sorted(versions)}, expected {tag.lstrip('v')}")
    version = versions.pop()
    lines = [
        "class Llmgate < Formula",
        '  desc "Gateway that lets Claude Code use any provider, account and model"',
        f'  homepage "https://github.com/{REPOSITORY}"',
        f'  version "{version}"',
        '  license "Apache-2.0"',
        "",
    ]
    for block, os_name in BLOCKS:
        entries = [(arch_block, arch) for arch_block, arch in ARCHES if (os_name, arch) in found]
        if not entries:
            continue
        lines.append(f"  on_{block} do")
        for arch_block, arch in entries:
            _, digest = found[(os_name, arch)]
            url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/llmgate-{version}-{os_name}-{arch}.tar.gz"
            lines += [f"    on_{arch_block} do", f'      url "{url}"', f'      sha256 "{digest}"', "    end"]
        lines.append("  end")
    lines += ["", "  def install", '    bin.install "llmgate"', "  end", "", "  test do", '    assert_match version.to_s, shell_output("#{bin}/llmgate --version")', "  end", "end", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sums")
    parser.add_argument("tag")
    args = parser.parse_args()
    with open(args.sums, encoding="utf-8") as handle:
        sys.stdout.write(render(handle.read(), args.tag))


if __name__ == "__main__":
    main()
