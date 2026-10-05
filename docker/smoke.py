#!/usr/bin/env python3
"""Check installed container tools on tiny inputs; no paper experiments."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, *, cwd, stdin=None):
    result = subprocess.run([str(arg) for arg in command], cwd=cwd, input=stdin,
                            text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(f'{command}\n{result.stdout}\n{result.stderr}')
    return result.stdout + result.stderr


def compiler_smoke(root):
    compiler = os.environ.get('SOUFFLE_BIN', 'souffle')
    facts = root / 'input'
    facts.mkdir()
    (facts / 'edge.facts').write_text('1\t2\n2\t3\n')
    (facts / 'edge.prob').write_text('0.6\n0.7\n')
    program = root / 'compute.dl'
    program.write_text('.decl edge(x:number,y:number)\n.input edge\n'
                       '.decl path(x:number,y:number)\n.output path\n'
                       'path(x,y) :- edge(x,y).\n'
                       'path(x,z) :- path(x,y), edge(y,z).\n')
    for kind in ('full', 'inc'):
        binary = root / kind
        run([compiler, f'--{kind}-only', '-F', facts, program, '-o', binary], cwd=root)
        for mode in (('plain', 'rewrite') if kind == 'full' else ('inc-naive', 'inc-regional', 'full')):
            output = root / f'{kind}-{mode}'
            output.mkdir()
            flags = ['--rewrite'] if mode == 'rewrite' else []
            if kind == 'inc':
                flags = ['--setmode', mode]
            run([binary, '-F', facts, '-D', output, *flags], cwd=root,
                stdin='q\n' if kind == 'inc' else None)
            probabilities = (output / 'facts.prob').read_text()
            match = re.search(r'path\(1,\s*3\)\s*:\s*([\d.eE+-]+)', probabilities)
            assert match and abs(float(match[1]) - 0.42) < 1e-8, probabilities
    print('Installed PSouffle: full/rewrite and online modes passed')


def engine_smoke(root):
    from problog.bdd_formula import BDD
    from problog.sdd_formula import SDD
    assert BDD.is_available() and SDD.is_available(), 'ProbLog BDD/SDD backends missing'
    program = root / 'smoke.pl'
    program.write_text('0.6::a.\n0.7::b.\nc :- a,b.\nquery(c).\n')
    assert '0.42' in run(['problog', program], cwd=root)

    # VProbLog's help exits nonzero in some revisions; verify the ELF and its
    # dependency resolution separately from a benchmark run.
    vlog = Path(os.environ['VLOG_BIN'])
    assert vlog.is_file() and os.access(vlog, os.X_OK), vlog
    assert 'not found' not in run(['ldd', vlog], cwd=root)
    result = subprocess.run([str(vlog), 'help'], capture_output=True, text=True, timeout=30)
    assert 'usage' in (result.stdout + result.stderr).lower(), (result.stdout, result.stderr)

    program = root / 'smoke.scl'
    program.write_text('rel 0.6::a(1)\nrel 0.7::b(1)\n'
                       'rel c(x) = a(x) and b(x)\n'
                       'rel d(z) = a(x), b(y), z == $band(6, 3)\nquery c\nquery d\n')
    output = run([os.environ['SCLI_BIN'], program, '-p', 'topkproofs', '--top-k', '1000000'], cwd=root)
    assert '0.42' in output and '(2)' in output, output
    print('ProbLog, VProbLog, and patched Scallop passed tool checks')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', action='store_true')
    parser.add_argument('--engines', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='psouffle-container-') as tmp:
        root = Path(tmp)
        if args.compiler:
            compiler_smoke(root)
        if args.engines:
            engine_smoke(root)


if __name__ == '__main__':
    main()
