#!/usr/bin/env python3
"""End-to-end execution capability and rewrite isolation checks."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

from run_full_regression_case import assert_prob_close, parse_prob_file

contribution_path = Path(__file__).resolve().parents[2] / 'evaluation/full/rewrite_contribution.py'
contribution_spec = importlib.util.spec_from_file_location('rewrite_contribution', contribution_path)
contribution = importlib.util.module_from_spec(contribution_spec)
contribution_spec.loader.exec_module(contribution)


def check_diagnostics(payload, *, rewrite=False):
    stages = {stage['name']: stage.get('info', {}) for stage in payload['turns'][0]['stages']}
    prune = stages['PRUNING']
    for kind in ('nodes', 'edges'):
        assert int(prune['before_prune_' + kind]) >= int(prune['after_prune_' + kind]), prune
    if not rewrite:
        return
    info = stages['FC_WMC_HYBRID']
    pattern_keys = ['rewrite_' + kind + '_regions' for kind in
                    ('all_facts', 'single', 'linear', 'parallel', 'fan_out', 'simple_fact')]
    assert sum(int(info[key]) for key in pattern_keys) == int(info['rewrite_simple_regions']), info
    total_regions = int(info['rewrite_simple_regions']) + int(info['rewrite_general_regions'])
    overlay_regions = sum(int(value) for key, value in info.items()
                          if key.startswith('implicit_overlay_') and key.endswith('_regions'))
    assert total_regions == int(info['graph_rewrite_rewritten_regions']) + overlay_regions, info
    row = contribution.values(prune, info)
    assert row is not None, info
    assert row['simple_regions'] + row['general_regions'] == total_regions, row
    for kind in ('nodes', 'edges'):
        before = int(prune['after_prune_' + kind])
        after = int(info['rewrite_final_' + kind])
        assert int(info['rewrite_simple_' + kind + '_net_removed']) + int(
            info['rewrite_general_' + kind + '_net_removed']) == before - after, info
    assert row['simple_nodes_removed'] + row['general_nodes_removed'] == (
        int(prune['after_prune_nodes']) - int(info['rewrite_final_nodes'])), row
    assert row['simple_net_edges_removed'] + row['general_net_edges_removed'] == (
        int(prune['after_prune_edges']) - int(info['rewrite_final_edges'])), row


def execution_log(out):
    paths = [path for path in out.glob('*.json')
             if not path.name.startswith('and-redundancy-')]
    assert len(paths) == 1, paths
    return json.loads(paths[0].read_text())


def check_and_redundancy(out, baseline, *, rewrite=False):
    phases = ['before-rewrite', 'after-rewrite'] if rewrite else ['before-rewrite']
    assert {path.name for path in out.glob('and-redundancy-*.json')} == {
        'and-redundancy-' + phase + '.json' for phase in phases}, out
    stages = {stage['name']: stage.get('info', {}) for stage in baseline['turns'][0]['stages']}
    for phase in phases:
        report = json.loads((out / ('and-redundancy-' + phase + '.json')).read_text())
        assert report['schema'] == 'and-input-redundancy-v1', report
        assert report['phase'] == phase, report
        for flag in ('complete_derivations', 'read_only', 'independent_certificates'):
            assert report[flag] is True, report
        stats = report['stats']
        for key in ('nodes', 'edges', 'input_associations', 'eligible_definitions',
                    'proven_input_associations', 'affected_edges', 'distinct_redundant_nodes',
                    'recursive_nodes', 'analysis_ms'):
            assert stats[key] >= 0, stats
        proofs = report['proofs']
        assert stats['proven_input_associations'] == len(proofs), report
        assert stats['affected_edges'] == len({proof['edge']['id'] for proof in proofs}), report
        assert stats['distinct_redundant_nodes'] == len({proof['redundant']['id']
                                                      for proof in proofs}), report
        for kind in ('nodes', 'edges'):
            if phase == 'before-rewrite':
                expected = stages['PRUNING']['after_prune_' + kind]
            else:
                expected = stages['FC_WMC_HYBRID']['rewrite_final_' + kind]
            assert stats[kind] == int(expected), (phase, stats, expected)


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
                        ('inc', ['--inc-only']),
                        ('baked', ['--rewrite', '--dump=and-redundancy'])]:
        binary = root / kind
        generated = root / (kind + '.cpp')
        run([args.souffle_bin, *flags, '-F', facts, '-D', default_out,
             root / 'compute.dl', '-g', generated])
        source = generated.read_text()
        if kind == 'full':
            assert 'stratum_inc_table_update' not in source
            assert '$inc_delta_tuple_delete_' not in source
            assert 'runFullPipeline(opt' in source
            assert 'runPipeline(opt' not in source
        if kind == 'inc':
            assert 'stratum_inc_table_update' in source
            assert 'runFullPipeline(opt' not in source
        if kind == 'baked':
            assert 'setDumpAndRedundancyEnabled(true)' in source
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
    plain_payload = json.loads(plain_log)
    check_diagnostics(plain_payload)
    assert not list(plain.glob('and-redundancy-*.json'))
    assert 'IO_LOAD_FULL' not in plain_log and 'CONSTRUCT_RULE_FULL' not in plain_log
    rewrite_payloads = {}
    for kind, label, flags in [
            ('hybrid', 'smart', ['--full-only', '--rewrite']),
            ('hybrid', 'explicit', ['--explicit-rewrite']),
            ('hybrid', 'implicit', ['--implicit-rewrite']),
            ('full', 'full_rewrite', ['--rewrite']),
            ('baked', 'baked_rewrite', [])]:
        out, _ = execute(kind, label, flags)
        assert_prob_close(plain / 'facts.prob', out / 'facts.prob', label=label)
        payload = execution_log(out)
        check_diagnostics(payload, rewrite=True)
        assert 'rewrite_impl' in json.dumps(payload), payload
        rewrite_payloads[label] = payload
        if kind == 'baked':
            check_and_redundancy(out, rewrite_payloads['full_rewrite'], rewrite=True)
        else:
            assert not list(out.glob('and-redundancy-*.json'))

    for kind, label, flags, baseline in [
            ('hybrid', 'and_plain', ['--dump=and-redundancy'], plain_payload),
            ('full', 'and_rewrite', ['--rewrite', '--dump=and-redundancy'],
             rewrite_payloads['full_rewrite'])]:
        out, _ = execute(kind, label, flags)
        assert_prob_close(plain / 'facts.prob', out / 'facts.prob', label=label)
        payload = execution_log(out)
        rewrite = label == 'and_rewrite'
        check_diagnostics(payload, rewrite=rewrite)
        check_and_redundancy(out, baseline, rewrite=rewrite)
        actual_stages = {stage['name']: stage.get('info', {})
                         for stage in payload['turns'][0]['stages']}
        baseline_stages = {stage['name']: stage.get('info', {})
                           for stage in baseline['turns'][0]['stages']}
        for kind in ('nodes', 'edges'):
            for prefix in ('before_prune_', 'after_prune_'):
                key = prefix + kind
                assert actual_stages['PRUNING'][key] == baseline_stages['PRUNING'][key]
            if rewrite:
                key = 'rewrite_final_' + kind
                assert actual_stages['FC_WMC_HYBRID'][key] == baseline_stages['FC_WMC_HYBRID'][key]

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

    # Capability validation must reject this standalone-only diagnostic before
    # input loading, even when the fact directory does not exist.
    missing_facts = root / 'missing_and_input'
    diagnostic_error = '--dump=and-redundancy requires standalone full execution'
    for kind, flags in [('hybrid', ['--online']), ('hybrid', ['--setmode', 'full']),
                        ('inc', [])]:
        out = root / ('and_online_invalid_' + kind + '_' + str(len(flags)))
        out.mkdir()
        result = run([binaries[kind], '-F', missing_facts, '-D', out,
                      '--dump=and-redundancy', *flags], stdin='q\n', success=False)
        assert diagnostic_error in result.stderr, result.stderr
        assert not list(out.iterdir()), out
    for flags in (['--online'], ['--inc-only'], ['--setmode', 'full']):
        generated = root / 'and_online_invalid.cpp'
        result = run([args.souffle_bin, '-F', missing_facts, '--dump=and-redundancy',
                      *flags, root / 'compute.dl', '-g', generated], success=False)
        assert diagnostic_error in result.stderr, result.stderr
        assert not generated.exists(), generated

    out = root / 'and_online_mutable_invalid'
    out.mkdir()
    result = run([binaries['hybrid'], '--online', '-F', facts, '-D', out],
                 stdin='set dump and-redundancy\nshow config\nq\n')
    assert 'dump and-redundancy requires standalone full execution' in result.stdout, result.stdout
    assert 'Enabled dump and-redundancy' not in result.stdout, result.stdout
    assert not list(out.glob('and-redundancy-*.json')), out

    aggregate = Path(__file__).parent / 'cases' / 'problog_sum_exact_roundtrip'
    binary = root / 'unsupported_online_aggregate'
    run([args.souffle_bin, '--inc-only', '-F', aggregate / 'input', '-D', default_out,
         aggregate / 'compute.dl', '-o', binary])
    result = run([binary], stdin='q\n', success=False)
    assert 'Aggregate replay requires standalone full execution' in result.stderr

    # Shared facts prevent local absorption, leaving a bounded general SISO
    # summary. z = (a & b) | (a & b & c), so its exact probability is 0.42.
    general_facts = root / 'general_input'
    general_facts.mkdir()
    for relation, probability in [('a', 0.6), ('b', 0.7), ('c', 0.2)]:
        (general_facts / (relation + '.facts')).write_text('1\n')
        (general_facts / (relation + '.prob')).write_text(str(probability) + '\n')
    general_program = root / 'general.dl'
    general_program.write_text(''.join(f'.decl {name}(k:number)\n' for name in 'abcxyz') +
                               '.input a\n.input b\n.input c\n.output z\n'
                               'x(k) :- a(k),b(k).\ny(k) :- a(k),b(k),c(k).\n'
                               'z(k) :- x(k).\nz(k) :- y(k).\n')
    general_binary = root / 'general'
    run([args.souffle_bin, '--full-only', '-F', general_facts, general_program, '-o', general_binary])
    for variant, flags in [('plain', []), ('explicit', ['--explicit-rewrite']),
                           ('implicit', ['--implicit-rewrite'])]:
        out = root / ('general_' + variant)
        out.mkdir()
        run([general_binary, '-F', general_facts, '-D', out, *flags])
        probabilities = parse_prob_file(out / 'facts.prob')
        assert abs(probabilities['z(1)'] - 0.42) < 1e-8, probabilities
        payload = json.loads(next(out.glob('*.json')).read_text())
        check_diagnostics(payload, rewrite=bool(flags))
        if flags:
            info = next(stage['info'] for stage in payload['turns'][0]['stages']
                        if stage['name'] == 'FC_WMC_HYBRID')
            assert int(info['rewrite_general_regions']) > 0, info
            assert int(info['graph_rewrite_general_nodes_removed']) > 0, info
            assert int(info['graph_rewrite_general_edges_removed']) > 0, info
            assert int(info['graph_rewrite_general_edges_added']) > 0, info
            if variant == 'implicit':
                assert float(info['implicit_graph_detect_ms']) > 0, info
                assert float(info['implicit_graph_rewrite_ms']) > 0, info
                assert float(info['implicit_total_ms']) >= float(info['implicit_graph_rewrite_ms']), info
    print('Execution capabilities, rewrite equivalence, and online isolation passed')


if __name__ == '__main__':
    main()
