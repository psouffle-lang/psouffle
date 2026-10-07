#!/usr/bin/env python3
"""End-to-end private series and terminal marginal factoring contracts."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess

from run_full_regression_case import assert_prob_close, parse_prob_file


SERIES = '--local-series-contraction'
TERMINAL = '--terminal-query-factors'
ALIAS = '--deterministic-event-aliases'
PASS_ERROR = (SERIES + ' and ' + TERMINAL + ' require standalone full inference')
OUTPUTS = ('a', 'shared', 'y', 'q', 'alias', 'protected', 'joint')


def run(command, *, success=True):
    result = subprocess.run([str(value) for value in command], text=True,
                            capture_output=True, timeout=200)
    if (result.returncode == 0) != success:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def stages(output):
    logs = list(output.glob('*.json'))
    assert len(logs) == 1, logs
    payload = json.loads(logs[0].read_text())
    info = {}
    order = []
    for stage in payload['turns'][0]['stages']:
        order.append(stage['name'])
        info.setdefault(stage['name'], {}).update(stage.get('info', {}))
    return info, order


def check_expected(values, observed):
    parent_probability = 1.0 if observed else 0.7
    fact_probability = 1.0 if observed else 0.25
    expected = {
        'a(1)': parent_probability,
        'shared(1)': 0.5 * parent_probability,
        'y(1)': 0.56 * fact_probability,
        'q(1)': 0.504 * fact_probability,
        'alias(1)': 0.504 * fact_probability,
        'protected(1)': 0.6 * parent_probability,
        'joint(1)': 0.6 * fact_probability,
    }
    assert set(values) == set(expected), (values, expected)
    for name, probability in expected.items():
        assert math.isfinite(values[name]) and abs(values[name] - probability) <= 1e-8, (
            observed, name, values[name], probability)


def check_passes(output, flags, *, require_hits=False):
    info, order = stages(output)
    if SERIES not in flags and TERMINAL not in flags:
        assert 'PRIVATE_FACTOR_REWRITE' not in order, order
        assert 'PRIVATE_FACTOR_PREPARATION' not in order, order
        assert 'LOCAL_SERIES_CONTRACTION' not in order, order
        assert 'TERMINAL_QUERY_FACTORS' not in order, order
        return
    assert order.count('PRIVATE_FACTOR_REWRITE') == 1, order
    assert not {'PRIVATE_FACTOR_PREPARATION', 'LOCAL_SERIES_CONTRACTION',
                'TERMINAL_QUERY_FACTORS'}.intersection(order), order
    preparation = order.index('PRIVATE_FACTOR_REWRITE')
    assert preparation > order.index('PRUNING'), order
    stage = info['PRIVATE_FACTOR_REWRITE']
    for key in ('collection_passes', 'scc_passes', 'support_passes'):
        assert int(stage['private_factor_' + key]) == 1, stage
    assert int(stage['private_factor_retirement_batches']) <= 1, stage
    if SERIES in flags:
        for key in ('local_series_contractions', 'local_series_removed_nodes',
                    'local_series_removed_edges', 'local_series_total_ms'):
            assert float(stage[key]) >= 0, stage
        if require_hits:
            assert int(stage['local_series_contractions']) >= 1, stage
    if TERMINAL in flags:
        for key in ('terminal_query_factored_queries', 'terminal_query_factored_output_queries',
                    'terminal_query_hidden_chain_steps', 'terminal_query_removed_nodes',
                    'terminal_query_removed_edges', 'terminal_query_total_ms'):
            assert float(stage[key]) >= 0, stage
        assert int(stage['terminal_query_factored_output_queries']) + int(
            stage['terminal_query_hidden_chain_steps']) == int(stage['terminal_query_factored_queries']), stage
        if require_hits:
            assert int(stage['terminal_query_factored_output_queries']) >= 1, stage
    hits = int(stage.get('local_series_contractions', 0)) + int(
        stage.get('terminal_query_factored_queries', 0))
    assert int(stage['private_factor_retirement_batches']) == int(hits > 0), stage
    assert int(stage['private_factor_terminal_view_commits']) == int(hits > 0), stage
    assert int(stage['private_factor_owner_nodes_before']) - int(
        stage['private_factor_retired_owner_nodes']) == int(stage['private_factor_owner_nodes_after']), stage
    assert int(stage['private_factor_owner_edges_before']) - int(
        stage['private_factor_retired_owner_edges']) + int(
        stage['private_factor_materialized_compound_edges']) == int(stage['private_factor_owner_edges_after']), stage
    for stage in ('FORWARD_COMPILATION', 'IO_DUMP'):
        if stage in order:
            assert order.index(stage) > preparation, order


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--souffle-bin', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root.resolve() / 'private_factor_passes'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    facts = root / 'input'
    facts.mkdir()
    for name, probability in (('f', 0.4), ('g', 0.6), ('h', 0.5)):
        (facts / (name + '.facts')).write_text('1\n')
        (facts / (name + '.prob')).write_text(str(probability) + '\n')
    source = (
        ''.join(f'.decl {name}(k:number)\n' for name in
                ('f', 'g', 'h', 'a', 'shared', 'mid', 'y', 'q', 'alias', 'protected', 'joint', 'obs')) +
        '.input f\n.input g\n.input h\n' +
        ''.join(f'.output {name}\n' for name in OUTPUTS) +
        'a(k) :- f(k).\na(k) :- g(k).\nshared(k) :- a(k),h(k).\n'
        '0.7::mid(k) :- a(k).\n0.8::y(k) :- mid(k),f(k).\n'
        '0.9::q(k) :- y(k).\nalias(k) :- q(k).\n'
        '0.6::protected(k) :- a(k).\njoint(k) :- protected(k),f(k).\n'
        '0.5::obs(k) :- f(k).\n')
    variants = [
        ('plain', []),
        ('series', [SERIES]),
        ('terminal', [TERMINAL]),
        ('both', [SERIES, TERMINAL]),
        ('alias_both', [ALIAS, SERIES, TERMINAL]),
        ('rewrite_both', ['--rewrite', SERIES, TERMINAL]),
        ('alias_rewrite_both', [ALIAS, '--rewrite', SERIES, TERMINAL]),
        ('alias_explicit_both', [ALIAS, '--explicit-rewrite', SERIES, TERMINAL]),
    ]
    for observed in (True, False):
        label = 'true' if observed else 'false'
        program = root / ('compute_' + label + '.dl')
        program.write_text(source + 'evidence(obs(1),' + label + ').\n')
        binary = root / ('compute_' + label)
        run([args.souffle_bin, '--full-only', '-F', facts, program, '-o', binary])
        baseline = None
        for variant, flags in variants:
            output = root / (label + '_' + variant)
            output.mkdir()
            run([binary, '-F', facts, '-D', output, *flags])
            check_expected(parse_prob_file(output / 'facts.prob'), observed)
            check_passes(output, flags, require_hits=variant == 'both')
            if baseline is None:
                baseline = output
            else:
                assert_prob_close(baseline / 'facts.prob', output / 'facts.prob',
                                  label=label + '/' + variant)
        if observed:
            generated = root / 'baked_private_factors.cpp'
            run([args.souffle_bin, '--full-only', SERIES, TERMINAL,
                 '-F', facts, program, '-g', generated])
            generated_source = generated.read_text()
            for setter in ('setLocalSeriesContractionEnabled(true)',
                           'setTerminalQueryFactorsEnabled(true)'):
                assert setter in generated_source, setter

            # Invalid scopes must fail during option validation before input
            # loading, even though this deliberately missing directory is used.
            missing_facts = root / 'missing_input'
            for index, flags in enumerate(([SERIES, '--derv-only'], [TERMINAL, '--derv-only'],
                                           [SERIES, TERMINAL, '--online'],
                                           [SERIES, TERMINAL, '--setmode=full'])):
                output = root / ('invalid_runtime_' + str(index))
                output.mkdir()
                result = run([binary, '-F', missing_facts, '-D', output, *flags], success=False)
                assert PASS_ERROR in result.stderr, result.stderr
                assert not list(output.iterdir()), output
            for index, mode in enumerate(('--online', '--inc-only', '--derv-only')):
                generated = root / ('invalid_scope_' + str(index) + '.cpp')
                result = run([args.souffle_bin, SERIES, TERMINAL, mode,
                              '-F', missing_facts, program, '-g', generated], success=False)
                assert PASS_ERROR in result.stderr, result.stderr
                assert not generated.exists(), generated
    print('private factor pass CLI regression passed')


if __name__ == '__main__':
    main()
