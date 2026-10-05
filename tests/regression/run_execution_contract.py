#!/usr/bin/env python3
"""End-to-end execution capability and rewrite isolation checks."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from run_full_regression_case import assert_prob_close, parse_prob_file


def run(args, *, stdin=None, success=True):
    result = subprocess.run([str(a) for a in args], input=stdin, text=True,
                            capture_output=True, timeout=180)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--souffle-bin', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root / 'execution_contract'
    if root.exists():
        shutil.rmtree(root)
    fixture = Path(__file__).parent / 'cases' / 'smoke_exact_inference'
    shutil.copytree(fixture, root)
    facts = root / 'input'
    default_out = root / 'compile_output'
    default_out.mkdir()

    binaries = {}
    for kind, flags in [('hybrid', []), ('full', ['--full-only']),
                        ('inc', ['--inc-only']), ('baked', ['--rewrite'])]:
        binary = root / kind
        generated = root / (kind + '.cpp')
        run([args.souffle_bin, *flags, '-F', facts, '-D', default_out,
             root / 'compute.dl', '-g', generated])
        source = generated.read_text()
        if kind == 'full':
            assert 'stratum_inc_table_update' not in source
            assert 'runFullPipeline(opt' in source
            assert 'runPipeline(opt' not in source
        if kind == 'inc':
            assert 'stratum_inc_table_update' in source
            assert 'runFullPipeline(opt' not in source
        run([args.souffle_bin, *flags, '-F', facts, '-D', default_out,
             root / 'compute.dl', '-o', binary])
        binaries[kind] = binary
        run([binary, '--help'])

    def execute(kind, label, flags, stdin=None, success=True):
        out = root / label
        out.mkdir()
        result = run([binaries[kind], '-F', facts, '-D', out,
                      '--logfile', label, *flags], stdin=stdin, success=success)
        return out, result

    plain, _ = execute('hybrid', 'plain', [])
    expected = parse_prob_file(plain / 'facts.prob')
    assert abs(expected['path(1,4)'] - 0.6032) < 1e-8, expected
    plain_log = next(plain.glob('*.json')).read_text()
    assert 'IO_LOAD_FULL' not in plain_log and 'CONSTRUCT_RULE_FULL' not in plain_log
    for kind, label, flags in [
            ('hybrid', 'smart', ['--full-only', '--rewrite']),
            ('hybrid', 'explicit', ['--explicit-rewrite']),
            ('hybrid', 'implicit', ['--implicit-rewrite']),
            ('full', 'full_rewrite', ['--rewrite']),
            ('baked', 'baked_rewrite', [])]:
        out, _ = execute(kind, label, flags)
        assert_prob_close(plain / 'facts.prob', out / 'facts.prob', label=label)
        payload = json.loads(next(out.glob('*.json')).read_text())
        assert 'rewrite_impl' in json.dumps(payload), payload

    for label, flags in [('merge', ['--merge-bi-imp']),
                         ('extra_prune', ['--prune-extra']),
                         ('graph_only_disabled', ['--derv-only=false'])]:
        out, _ = execute('full', label, flags)
        assert_prob_close(plain / 'facts.prob', out / 'facts.prob', label=label)
    graph, _ = execute('full', 'graph_only', ['--derv-only', '--rewrite', '--dumpjson'])
    assert (graph / 'derivation.json').exists()
    assert not (graph / 'facts.prob').exists()
    generated = root / 'derivation_only.cpp'
    run([args.souffle_bin, '--derv-only', root / 'compute.dl', '-g', generated])
    assert 'setDerivationOnly(true)' in generated.read_text()

    script = 'insert 0.5::edge(2,4)\ncommit\nsetmode full\ndelete edge(1,4)\ncommit\nq\n'
    online = []
    for kind, label, flags in [('hybrid', 'online_naive', ['--inc-only']),
                               ('hybrid', 'online_regional', ['--setmode', 'inc-regional']),
                               ('hybrid', 'online_full', ['--setmode', 'full']),
                               ('inc', 'inc_default', [])]:
        baseline, _ = execute(kind, label + '_baseline', flags, stdin='q\n')
        assert_prob_close(plain / 'facts.prob', baseline / 'facts.prob', label=label + ' baseline')
        out, _ = execute(kind, label, flags, stdin=script)
        online.append(out)
        for path in [*baseline.glob('*.json'), *out.glob('*.json')]:
            text = path.read_text()
            assert 'rewrite_impl' not in text and 'FC_WMC_HYBRID' not in text, path
    for out in online[1:]:
        assert_prob_close(online[0] / 'facts.prob', out / 'facts.prob', label=out.name)

    invalid = [
        ('hybrid', ['--inc-only', '--rewrite']),
        ('hybrid', ['--rewrite', '--inc-only']),
        ('hybrid', ['--setmode', 'full', '--implicit-rewrite']),
        ('hybrid', ['--full-only', '--inc-only']),
        ('hybrid', ['--inc-only', '--full-only']),
        ('hybrid', ['--full-only', '--setmode', 'inc-naive']),
        ('hybrid', ['--explicit-rewrite', '--implicit-rewrite']),
        ('hybrid', ['--implicit-rewrite', '--explicit-rewrite']),
        ('full', ['--inc-only']),
        ('inc', ['--full-only']),
        ('baked', ['--setmode', 'full']),
        ('hybrid', ['--online', '--derv-only']),
        ('hybrid', ['--online', '--merge-bi-imp']),
        ('hybrid', ['--online', '--prune-extra']),
    ]
    for index, (kind, flags) in enumerate(invalid):
        out, _ = execute(kind, 'invalid_' + str(index), flags, success=False)
        assert not (out / 'facts.prob').exists()
    for flags in [['--full-only', '--inc-only'], ['--inc-only', '--full-only'],
                  ['--full-only', '--setmode', 'full'], ['--inc-only', '--rewrite'],
                  ['--online', '--rewrite'], ['--explicit-rewrite', '--implicit-rewrite'],
                  ['--inc-only', '--derv-only']]:
        run([args.souffle_bin, *flags, root / 'compute.dl', '-g', root / 'invalid.cpp'], success=False)

    aggregate = Path(__file__).parent / 'cases' / 'problog_sum_exact_roundtrip'
    binary = root / 'unsupported_online_aggregate'
    run([args.souffle_bin, '--inc-only', '-F', aggregate / 'input', '-D', default_out,
         aggregate / 'compute.dl', '-o', binary])
    result = run([binary], stdin='q\n', success=False)
    assert 'Aggregate replay requires standalone full execution' in result.stderr
    print('Execution capabilities, rewrite equivalence, and online isolation passed')


if __name__ == '__main__':
    main()
