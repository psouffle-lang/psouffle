#!/usr/bin/env python3
"""Check compiler recursion attestation and aggregate namespace exclusions."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--souffle-bin', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root / 'compiler_acyclicity'
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)

    def generate(name, program, expected_attestation):
        source = root / (name + '.dl')
        generated = root / (name + '.cpp')
        source.write_text(program)
        result = subprocess.run([str(args.souffle_bin), '--full-only', '-g', str(generated),
                                 str(source)], text=True, capture_output=True, timeout=60)
        assert result.returncode == 0, (name, result.stdout, result.stderr)
        text = generated.read_text()
        managers = re.findall(r'ruleManager = RuleManager\([^\n]*, (true|false)\);', text)
        assert managers == [expected_attestation], (name, managers)
        return text

    prefix = '.decl source_facts(x:number)\n.input source_facts\n.decl result(x:number)\n.output result\n'
    generate('ordinary', prefix + 'result(x) :- source_facts(x).\n', 'true')
    generate('aggregate', '.decl w(x:number,v:number)\n.input w\n'
             '.decl base(x:number)\nbase(1).\n'
             '.decl total(x:number,s:number)\n.output total\n'
             'total(X,S) :- base(X), S = sum V : { w(X,V) }.\n', 'true')
    generate('reserved_state', prefix +
             '.decl __agg_sum_state(x:number)\n.input __agg_sum_state\n'
             'result(x) :- source_facts(x), __agg_sum_state(x).\n', 'false')
    generate('eqrel', '.decl equivalence(x:number,y:number) eqrel\n.input equivalence\n'
             '.decl result(x:number,y:number)\n.output result\n'
             'result(x,y) :- equivalence(x,y).\n', 'false')

    recursive = generate('recursive', prefix +
                         '.decl step(x:number,y:number)\n.input step\n'
                         'result(x) :- source_facts(x).\n'
                         'result(y) :- result(x), step(x,y).\n', 'true')
    # Compiler attestation states that the emitted flags are trustworthy; the
    # runtime getter separately rejects both clauses of a recursive stratum.
    flags = re.findall(r'const Rule [^\n]*, ([01]), ([01]), (?:true|false), \{[^}]*\}\);',
                       recursive)
    assert ('0', '1') in flags and ('1', '1') in flags, flags


if __name__ == '__main__':
    main()
