#!/usr/bin/env python3
"""Pin VProbLog's recursive ExternalProject git-clone commands."""
from pathlib import Path
import subprocess
import sys


PINS = {
    'https://github.com/karmaresearch/trident.git': '621ed49c648c811d5f69617833539f419766d3be',
    'https://github.com/karmaresearch/kognac.git': 'ec961644647e2b545cfb859148cde3dff94d317e',
    'https://github.com/sparsehash/sparsehash.git': '1dffea3d917445d70d33d0c7492919fc4408fe5c',
    'https://github.com/Cyan4973/lz4.git': '0774d05537f9762f838f7ab541b7765f1a729cb5',
}


def main():
    args = sys.argv[1:]
    result = subprocess.run(['/usr/bin/git', *args])
    if result.returncode:
        return result.returncode
    if args and args[0] == 'clone':
        for url, revision in PINS.items():
            if url in args:
                index = args.index(url)
                destination = args[index + 1] if index + 1 < len(args) else Path(url).name.removesuffix('.git')
                return subprocess.run(['/usr/bin/git', '-C', destination, 'checkout', revision]).returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
