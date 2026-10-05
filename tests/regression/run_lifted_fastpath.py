#!/usr/bin/env python3
"""Exact pointwise fastpath, correlation, fallback, and CLI contracts."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from run_full_regression_case import assert_prob_close, parse_prob_file


def run(command, *, success=True):
    result = subprocess.run([str(arg) for arg in command], text=True,
                            capture_output=True, timeout=180)
    assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--souffle-bin', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root / 'lifted_fastpath'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    facts = root / 'input'
    facts.mkdir()
    for relation, rows, weights in [
            ('event', '1\n2\n', '0.6\n0.8\n'),
            ('gate', '1\n2\n', '0.7\n0.9\n'),
            ('pair', '1\t1\n2\t2\n', None),
            ('step', '1\t2\n', '0.5\n')]:
        (facts / (relation + '.facts')).write_text(rows)
        if weights:
            (facts / (relation + '.prob')).write_text(weights)
    pointwise = (
        '.decl event(k:number)\n.input event\n'
        '.decl gate(k:number)\n.input gate\n'
        '.decl pair(x:number,y:number)\n.input pair\n'
        '.decl prod(k:number)\n.output prod\n'
        '.decl duplicate(k:number)\n.output duplicate\n'
        '.decl alternative(k:number)\n.output alternative\n'
        '.decl weighted(k:number)\n.output weighted\n'
        'prod(k) :- event(k),gate(k).\n'
        'duplicate(x) :- event(x),pair(x,y),event(y).\n'
        'alternative(k) :- event(k).\nalternative(k) :- gate(k).\n'
        '0.5::weighted(k) :- event(k).\n')

    def compile_program(name, text, flags=()):
        program = root / (name + '.dl')
        program.write_text(text)
        binary = root / name
        run([args.souffle_bin, '-F', facts, *flags, program, '-o', binary])
        return binary

    def execute(binary, label, flags=()):
        output = root / ('out_' + label)
        output.mkdir()
        result = run([binary, '-F', facts, '-D', output, *flags])
        payload = json.loads(next(output.glob('*.json')).read_text())
        stages = {stage['name']: stage.get('info', {}) for stage in payload['turns'][0]['stages']}
        return output, stages, result

    full = compile_program('pointwise', pointwise)
    plain, _, _ = execute(full, 'plain')
    expected = parse_prob_file(plain / 'facts.prob')
    for key, probability in {'prod(1)': 0.42, 'prod(2)': 0.72,
                             'duplicate(1)': 0.6, 'duplicate(2)': 0.8,
                             'alternative(1)': 0.88, 'alternative(2)': 0.98,
                             'weighted(1)': 0.3, 'weighted(2)': 0.4}.items():
        assert abs(expected[key] - probability) < 1e-8, expected
    flags = ['--lifted-wmc', '--lifted-threshold=1']
    lifted, stages, _ = execute(full, 'complete', [*flags, '--rewrite', '--dump=dot,stat'])
    assert_prob_close(plain / 'facts.prob', lifted / 'facts.prob', label='complete lift')
    info = stages['LIFTED_WMC']
    assert info['lifted_handled'] == 'true' and info['lifted_complete'] == 'true', info
    assert int(info['lifted_output_tuples']) == 8, info
    assert int(info['lifted_closed_form_tuples']) == 4, info
    assert 'CREATE_GRAPH' not in stages and 'FC_WMC_HYBRID' not in stages, stages
    assert (lifted / 'abstract-derivation-graph.dot').exists()
    assert (lifted / 'abstract-derivation-graph.tsv').exists()
    threshold, stages, _ = execute(full, 'threshold', ['--lifted-wmc'])
    assert stages['LIFTED_WMC']['lifted_handled'] == 'false', stages
    assert 'CREATE_GRAPH' in stages, stages
    assert_prob_close(plain / 'facts.prob', threshold / 'facts.prob', label='threshold fallback')
    baked = compile_program('baked', pointwise, ['--full-only', '--lifted-wmc', '--lifted-threshold=0001'])
    output, stages, _ = execute(baked, 'baked')
    assert stages['LIFTED_WMC']['lifted_complete'] == 'true', stages
    assert_prob_close(plain / 'facts.prob', output / 'facts.prob', label='baked lift')

    # The recursive output shares event(k) with lifted prod(k). Keeping its
    # concrete provenance gives 0.42, rather than multiplying by 0.6 again.
    partial_program = pointwise + (
        '.decl step(x:number,y:number)\n.input step\n'
        '.decl reach(k:number)\n.output reach\n'
        'reach(k) :- prod(k),event(k).\nreach(y) :- reach(x),step(x,y).\n')
    partial = compile_program('partial', partial_program, ['--full-only'])
    baseline, _, _ = execute(partial, 'partial_plain')
    probabilities = parse_prob_file(baseline / 'facts.prob')
    assert abs(probabilities['reach(1)'] - 0.42) < 1e-8, probabilities
    assert abs(probabilities['reach(2)'] - 0.7788) < 1e-8, probabilities
    for label, rewrite in [('plain', []), ('explicit', ['--explicit-rewrite']),
                           ('implicit', ['--implicit-rewrite'])]:
        output, stages, _ = execute(partial, 'partial_' + label + '_lift', [*flags, *rewrite])
        info = stages['LIFTED_WMC']
        assert info['lifted_handled'] == 'true' and info['lifted_complete'] == 'false', info
        assert 'recursive' in info['lifted_rejected_reach'], info
        assert_prob_close(baseline / 'facts.prob', output / 'facts.prob', label='partial ' + label)

    # An explicit query on a non-output relation must survive complete lift.
    query = compile_program('query', pointwise + 'query(event(_)).\n', ['--full-only'])
    baseline, _, _ = execute(query, 'query_plain')
    output, stages, _ = execute(query, 'query_lift', flags)
    assert stages['LIFTED_WMC']['lifted_complete'] == 'false', stages
    assert_prob_close(baseline / 'facts.prob', output / 'facts.prob', label='extra query')

    multi = compile_program('multi',
        '.decl event(k:number)\n.input event\n'
        '.decl many(x:number,y:number)\nmany(1,1).\nmany(1,2).\n'
        '.decl result(x:number)\n.output result\nresult(x) :- many(x,y),event(y).\n',
        ['--full-only'])
    baseline, _, _ = execute(multi, 'multi_plain')
    output, stages, _ = execute(multi, 'multi_lift', flags)
    assert stages['LIFTED_WMC']['lifted_handled'] == 'false', stages
    assert 'multiple runtime witnesses' in stages['LIFTED_WMC']['lifted_rejected_result'], stages
    assert_prob_close(baseline / 'facts.prob', output / 'facts.prob', label='multi-witness fallback')

    evidence = compile_program('evidence', pointwise + 'evidence(event(1),true).\n', ['--full-only'])
    baseline, _, _ = execute(evidence, 'evidence_plain')
    output, stages, _ = execute(evidence, 'evidence_lift', flags)
    assert stages['LIFTED_WMC']['lifted_reason'] == 'evidence_present', stages
    assert_prob_close(baseline / 'facts.prob', output / 'facts.prob', label='evidence fallback')

    for invalid in ('-1', '1x', '18446744073709551616'):
        assert 'Invalid value for --lifted-threshold' in run(
            [full, '--lifted-threshold=' + invalid], success=False).stderr
        result = run([args.souffle_bin, '--lifted-threshold=' + invalid, root / 'pointwise.dl'],
                     success=False)
        assert 'Invalid value for --lifted-threshold' in result.stderr, result
    for selection in ('--inc-only', '--online', '--setmode=full'):
        assert 'requires standalone full execution' in run([full, '--lifted-wmc', selection],
                                                           success=False).stderr
        result = run([args.souffle_bin, '--lifted-wmc', selection, root / 'pointwise.dl'],
                     success=False)
        assert 'requires standalone full execution' in result.stderr, result
    print('Naive lifted fastpath, shared events, exact fallback, and CLI contracts passed')


if __name__ == '__main__':
    main()
