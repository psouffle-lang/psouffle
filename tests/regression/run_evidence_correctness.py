#!/usr/bin/env python3
"""Conditional inference against tiny possible-world oracles in every full mode."""
import argparse
from itertools import product
import math
from pathlib import Path
import shutil
import struct
import subprocess

from run_full_regression_case import parse_prob_file


MODES = {
    'plain': [],
    'explicit': ['--explicit-rewrite'],
    'implicit': ['--implicit-rewrite'],
    'auto': ['--rewrite'],
    'and': ['--and-input-redundancy'],
    'and_rewrite': ['--and-input-redundancy', '--rewrite'],
    'prune': ['--implicit-rewrite', '--prune-extra'],
    'lift': ['--lifted-wmc', '--lifted-threshold=0', '--rewrite'],
}


def posterior(weights, evidence, queries):
    denominator = 0.0
    numerators = dict.fromkeys(queries, 0.0)
    for world in product((False, True), repeat=len(weights)):
        mass = math.prod(p if value else 1 - p for p, value in zip(weights, world))
        if evidence(world):
            denominator += mass
            for key, query in queries.items():
                if query(world):
                    numerators[key] += mass
    assert denominator > 0
    return {key: mass / denominator for key, mass in numerators.items()}


def run(command, *, success=True):
    result = subprocess.run([str(arg) for arg in command], text=True,
                            capture_output=True, timeout=180)
    assert result.returncode >= 0, (command, result.returncode, result.stderr)
    assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--souffle-bin', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    ram32 = 'Word size: 32 bits' in run([args.souffle_bin, '--version']).stdout
    root = args.work_root / 'evidence_correctness'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)

    def compile_program(name, text, inputs):
        case = root / name
        case.mkdir()
        facts = case / 'input'
        facts.mkdir()
        for relation, (rows, weights) in inputs.items():
            (facts / (relation + '.facts')).write_text(rows)
            if weights is not None:
                (facts / (relation + '.prob')).write_text(weights)
        program = case / 'compute.dl'
        program.write_text(text)
        binary = case / 'compute'
        result = run([args.souffle_bin, '--full-only', '-F', facts, program, '-o', binary])
        (case / 'compile.log').write_text(result.stdout + result.stderr)
        return binary, facts

    def check(binary, facts, expected=None, *, label='default', error=None):
        for mode, flags in MODES.items():
            output = binary.parent / ('out_' + label + '_' + mode)
            output.mkdir()
            result = run([binary, '-F', facts, '-D', output, *flags], success=error is None)
            (output / 'run.log').write_text(result.stdout + result.stderr)
            if error:
                assert error in result.stdout + result.stderr, result
                assert not (output / 'facts.prob').exists(), output
                continue
            probabilities = parse_prob_file(output / 'facts.prob')
            assert probabilities.keys() == expected.keys(), (binary, mode, probabilities, expected)
            for key, value in expected.items():
                assert math.isclose(probabilities[key], value, rel_tol=0, abs_tol=1e-8), (
                    binary, mode, key, probabilities[key], value)

    inputs = {'a': ('1\n', '0.2\n'), 'b': ('1\n', '0.3\n')}
    # obs is a removable relation copy with no query/output declaration.
    # empty has no clauses. Both remain available as evidence targets.
    disjunction = (
        '.decl a(x:number)\n.input a\n.output a\nquery(a(_)).\n'
        '.decl b(x:number)\n.input b\n.output b\nquery(b(_)).\n'
        '.decl helper(x:number)\nhelper(X) :- a(X).\nhelper(X) :- b(X).\n'
        '.decl obs(x:number)\nobs(X) :- helper(X).\n'
        '.decl alias(x:number)\nalias(X) :- helper(X).\n'
        '.decl empty(x:number)\nevidence(empty(1),false).\n'
        'evidence(a(2),false).\n')
    queries = {'a(1)': lambda w: w[0], 'b(1)': lambda w: w[1]}
    for value in (True, False):
        binary, facts = compile_program('or_' + str(value).lower(), disjunction +
            f'evidence(obs(1),{str(value).lower()}).\n'
            f'evidence(alias(1),{str(value).lower()}).\n', inputs)
        expected = posterior([0.2, 0.3], lambda w: (w[0] or w[1]) == value, queries)
        check(binary, facts, expected)

    # Nullary heads must record every clause and witness, even after their RAM
    # relation becomes nonempty. Recursive proofs use the same fixed-point guard.
    nullary = (
        '.decl a(x:number)\n.input a\n.output a\nquery(a(_)).\n'
        '.decl b(x:number)\n.input b\n.output b\nquery(b(_)).\n'
        '.decl obs()\nobs() :- a(1).\nobs() :- b(1).\n')
    for recursive in (False, True):
        for value in (True, False):
            extra = ('.decl echo()\nquery(echo()).\n'
                     '0.4::echo() :- obs().\nobs() :- echo().\n' if recursive else '')
            binary, facts = compile_program(f'nullary_{recursive}_{value}', nullary + extra +
                f'evidence(obs(),{str(value).lower()}).\n', inputs)
            nullary_queries = dict(queries)
            if recursive:
                nullary_queries['echo()'] = lambda w: (w[0] or w[1]) and w[2]
            expected = posterior([0.2, 0.3, 0.4] if recursive else [0.2, 0.3],
                lambda w: (w[0] or w[1]) == value, nullary_queries)
            check(binary, facts, expected)

    for value in (True, False):
        binary, facts = compile_program('nullary_witness_' + str(value).lower(),
            '.decl a(x:number)\n.input a\n.output a\nquery(a(_)).\n'
            '.decl obs()\n0.4::obs() :- a(X).\n' +
            f'evidence(obs(),{str(value).lower()}).\n',
            {'a': ('1\n2\n', '0.2\n0.3\n')})
        expected = posterior([0.2, 0.3, 0.4, 0.4],
            lambda w: ((w[0] and w[2]) or (w[1] and w[3])) == value,
            {'a(1)': lambda w: w[0], 'a(2)': lambda w: w[1]})
        check(binary, facts, expected)

    weighted = disjunction.replace('helper(X) :- a(X).',
        '0.4::helper(X) :- a(X).\n0.5::helper(X) :- a(X).')
    binary, facts = compile_program('duplicate_probabilistic_rules', weighted +
        'evidence(obs(1),true).\nevidence(alias(1),true).\n', inputs)
    expected = posterior([0.2, 0.3, 0.4, 0.5],
        lambda w: (w[0] and (w[2] or w[3])) or w[1], queries)
    check(binary, facts, expected)

    shared = (
        '.decl base(x:number)\n.input base\n'
        '.decl ids(x:number)\n.input ids\n'
        '.decl obs(x:number)\n'
        'obs(X) :- ids(X),base(1),base(2).\nobs(X) :- ids(X),base(3).\n'
        '.decl q(x:number)\n.output q\nquery(q(_)).\n'
        '0.7::q(X) :- ids(X),base(1),base(3).\n'
        'evidence(obs(1),true).\n')
    binary, facts = compile_program('shared_event', shared,
        {'base': ('1\n2\n3\n', '0.2\n0.3\n0.4\n'), 'ids': ('1\n', None)})
    expected = posterior([0.2, 0.3, 0.4, 0.7],
        lambda w: (w[0] and w[1]) or w[2], {'q(1)': lambda w: w[0] and w[2] and w[3]})
    check(binary, facts, expected)

    binary, facts = compile_program('joint_impossible', disjunction +
        'evidence(obs(1),true).\nevidence(a(1),false).\nevidence(b(1),false).\n', inputs)
    check(binary, facts, error='Inconsistent evidence')
    binary, facts = compile_program('conflicting_values', disjunction +
        'evidence(a(1),true).\nevidence(a(1),false).\n', inputs)
    check(binary, facts, error='Inconsistent evidence')

    # A disconnected evidence component must be evaluated even under extra pruning.
    binary, facts = compile_program('independent', disjunction +
        '.decl d(x:number)\n.input d\nevidence(d(1),true).\n',
        {**inputs, 'd': ('1\n', '0.4\n')})
    check(binary, facts, {'a(1)': 0.2, 'b(1)': 0.3})
    (facts / 'd.prob').write_text('0\n')
    check(binary, facts, label='zero', error='Inconsistent evidence')
    (facts / 'd.facts').write_text('')
    (facts / 'd.prob').write_text('')
    check(binary, facts, label='absent_true', error='Inconsistent evidence')

    # Provenance elision must not erase deterministic observation roots,
    # including nullary facts and tuples outside every query component.
    deterministic = (
        '.decl ids(x:number)\n.input ids\n'
        '.decl obs(x:number)\nobs(X) :- ids(X).\n'
        '.decl present()\npresent().\n'
        '.decl q(x:number)\n.input q\n.output q\nquery(q(_)).\n'
        'evidence(present(),true).\nevidence(obs(2),false).\n')
    for value in (True, False):
        binary, facts = compile_program('deterministic_' + str(value).lower(), deterministic +
            f'evidence(obs(1),{str(value).lower()}).\n',
            {'ids': ('1\n', None), 'q': ('1\n', '0.2\n')})
        if value:
            check(binary, facts, {'q(1)': 0.2})
        else:
            check(binary, facts, error='Inconsistent evidence')

    # RAM still derives zero-weight heads. Their descendants must retain the
    # zero event instead of treating those heads as deterministic true facts.
    for zero_source in ('fact', 'rule'):
        prefix = ('.decl a(x:number)\n.input a\n'
                  '.decl obs(x:number)\n' +
                  ('obs(X) :- a(X).\n' if zero_source == 'fact' else
                   '0.0::obs(X) :- a(X).\n') +
                  '.decl q(x:number)\n.output q\nquery(q(_)).\n'
                  '0.5::q(X) :- obs(X).\n')
        for value in (True, False):
            binary, facts = compile_program('zero_' + zero_source + '_' + str(value).lower(), prefix +
                f'evidence(obs(1),{str(value).lower()}).\n',
                {'a': ('1\n', '0\n' if zero_source == 'fact' else None)})
            if value:
                check(binary, facts, error='Inconsistent evidence')
            else:
                check(binary, facts, {'q(1)': 0.0})

    conjunction = ','.join(f'a({i})' for i in range(8))
    binary, facts = compile_program('underflow',
        '.decl a(x:number)\n.input a\n.decl b(x:number)\n.input b\n'
        '.decl q(x:number)\n.output q\nquery(q(_)).\n' +
        f'q(1) :- {conjunction}.\nq(2) :- {conjunction},b(1).\n' +
        ''.join(f'evidence(a({i}),true).\n' for i in range(8)),
        {'a': (''.join(f'{i}\n' for i in range(8)), '1e-50\n' * 8), 'b': ('1\n', '0.25\n')})
    # P(E)=1e-400, but the two posteriors are representable and exact.
    check(binary, facts, {'q(1)': 1.0, 'q(2)': 0.25})

    binary, facts = compile_program('derived_underflow',
        '.decl a(x:number)\n.input a\n.decl b(x:number)\n.input b\n'
        '.decl ids(x:number)\n.input ids\n'
        '.decl obs(x:number)\n' + f'obs(1) :- {conjunction}.\n' +
        f'0.{"0" * 49}1::obs(2) :- ids(1).\n0.{"0" * 49}2::obs(2) :- ids(1).\n'
        '.decl q(x:number)\n.output q\nquery(q(_)).\n'
        'q(1) :- obs(1).\nq(2) :- obs(1),b(1).\nq(3) :- obs(2).\n'
        'evidence(obs(1),true).\nevidence(obs(2),true).\n',
        {'a': (''.join(f'{i}\n' for i in range(8)), '1e-50\n' * 8),
         'b': ('1\n', '0.25\n'), 'ids': ('1\n', None)})
    check(binary, facts, {'q(1)': 1.0, 'q(2)': 0.25, 'q(3)': 1.0})

    # Legal typed ground literals, including the unsigned 32-bit boundary,
    # signed minimum, floats and symbols. This program has no clauses.
    binary, facts = compile_program('typed_input_only',
        '.type U <: unsigned\n.type S <: symbol\n'
        '.decl u(x:U)\n.input u\n.output u\nquery(u(_)).\n'
        '.decl n(x:number)\n.input n\n.output n\nquery(n(_)).\n'
        '.decl f(x:float)\n.input f\n.output f\nquery(f(_)).\n'
        '.decl s(x:S)\n.input s\n.output s\nquery(s(_)).\n'
        'evidence(u(4294967295),true).\nevidence(n(-2147483648),true).\n'
        'evidence(f(-1.25),true).\nevidence(s("hello"),true).\n',
        {'u': ('4294967295\n', '0.2\n'), 'n': ('-2147483648\n', '0.3\n'),
         'f': ('-1.25\n', '0.4\n'), 's': ('hello\n', '0.5\n')})
    # Probability output renders numeric fields as signed RAM words.
    float_word = struct.unpack('=i' if ram32 else '=q',
                               struct.pack('=f' if ram32 else '=d', -1.25))[0]
    unsigned_word = -1 if ram32 else 4294967295
    check(binary, facts, {f'u({unsigned_word})': 1, 'n(-2147483648)': 1,
                         f'f({float_word})': 1, 's("hello")': 1})

    invalid = {
        'variable': ('number', 'X'),
        'expression': ('number', '1+1'),
        'symbol_type': ('symbol', '1'),
        'number_type': ('number', '"hello"'),
        'signed_overflow': ('number', '18446744073709551616'),
        'unsigned_overflow': ('unsigned', '18446744073709551616'),
        'unsigned_negative': ('unsigned', '-1'),
        'unsigned_for_signed': ('number', '1u'),
        'unsigned_for_float': ('float', '1u'),
        'float_overflow': ('float', '1' + '0' * 400 + '.0'),
        'record': ('R', '[1]'),
    }
    if ram32:
        invalid['unsigned_narrowing'] = ('unsigned', '4294967296')
    for name, (kind, literal) in invalid.items():
        program = root / (name + '.dl')
        program.write_text(('.type R = [x:number]\n' if kind == 'R' else '') +
            f'.decl a(x:{kind})\n.input a\n.output a\nevidence(a({literal}),true).\n')
        result = run([args.souffle_bin, program, '-g', root / (name + '.cpp')], success=False)
        assert 'Evidence arguments must be ground primitive literals' in result.stderr, result
    print('Evidence posterior oracles, pruning, impossibility, underflow and literal checks passed')


if __name__ == '__main__':
    main()
