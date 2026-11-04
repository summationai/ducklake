#!/usr/bin/env python3
"""Turns the profiles written by coverage.sh into docs/branching/COVERAGE.md.

Usage: coverage_report.py <profile dir> <instrumented binary> <report path>
Exits 1 when a function in src/branching is never executed, line coverage is below the target, or an uncovered
line has no reason in scripts/branching/coverage_exclusions.txt.
"""
import datetime
import glob
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

LINE_TARGET = 90.0
EXCLUSIONS = 'scripts/branching/coverage_exclusions.txt'
OUR_PREFIXES = ('src/branching/', 'src/include/branching/')
CONFIGS = ['default', 'no_inline', 'deletion_vectors', 'sqlite', 'ducklake_version']


def run(cmd, stdin=None):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, input=stdin).stdout


def relative(path):
    return os.path.relpath(path, os.getcwd())


def is_ours(path):
    return relative(path).startswith(OUR_PREFIXES)


def merge(profiles, target):
    run(['xcrun', 'llvm-profdata', 'merge', '-sparse', *profiles, '-o', target])


def export_functions(binary, profdata, sources):
    data = json.loads(run(['xcrun', 'llvm-cov', 'export', binary, '-instr-profile=' + profdata, *sources]))
    result = []
    for function in data['data'][0]['functions']:
        regions = function['regions']
        if not regions:
            continue
        file_id = regions[0][5]
        path = function['filenames'][file_id]
        if not is_ours(path):
            continue
        lines = [r[0] for r in regions if r[5] == file_id] + [r[2] for r in regions if r[5] == file_id]
        result.append(
            {
                'name': function['name'],
                'count': function['count'],
                'file': relative(path),
                'start': min(lines),
                'end': max(lines),
            }
        )
    return result


def demangle(names):
    mangled = [name.split(':', 1)[1] if ':_Z' in name else name for name in names]
    output = run(['xcrun', 'c++filt', '-n'], stdin='\n'.join(mangled) + '\n').splitlines()
    return [display_name(name) for name in output]


def display_name(name):
    name = name.replace('(anonymous namespace)::', '').replace('duckdb::', '')
    depth, cut = 0, len(name)
    for i, ch in enumerate(name):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == '(' and depth == 0:
            cut = i
            break
    base = name[:cut]
    rest = name[cut:]
    if '::$_' in rest or "'lambda" in rest:
        return base + ' (lambda)'
    return base


def line_counts(binary, profdata, source):
    """Line number -> execution count for the code lines of one source file (None for non-code lines)."""
    text = run(
        [
            'xcrun',
            'llvm-cov',
            'show',
            binary,
            '-instr-profile=' + profdata,
            '-show-instantiations=false',
            '-format=text',
            source,
        ]
    )
    counts = {}
    for line in text.splitlines():
        match = re.match(r'^\s*(\d+)\|\s*([0-9.]+[kMGTE]?)?\|(.*)$', line)
        if not match:
            continue
        number, count, code = int(match.group(1)), match.group(2), match.group(3)
        counts[number] = (None if count is None else (0 if count == '0' else 1), code)
    return counts


def load_exclusions():
    rules = []
    if os.path.exists(EXCLUSIONS):
        for raw in open(EXCLUSIONS):
            raw = raw.strip()
            if not raw or raw.startswith('#'):
                continue
            path, function, snippet, reason = [part.strip() for part in raw.split(' | ', 3)]
            rules.append({'path': path, 'function': function, 'snippet': snippet, 'reason': reason, 'used': False})
    return rules


def reason_for(path, function, code_lines, rules):
    text = '\n'.join(code_lines)
    for rule in rules:
        if rule['path'] == path and rule['function'] == function and rule['snippet'] in text:
            rule['used'] = True
            return rule['reason']
    if 'InternalException' in text:
        return 'internal invariant; not reachable through SQL'
    return None


def upstream_call_sites():
    sites = defaultdict(set)
    for path in glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/include/**/*.hpp', recursive=True):
        if is_ours(path):
            continue
        for match in re.finditer(r'DuckLakeBranching::(\w+)\(', open(path).read()):
            sites[match.group(1)].add(relative(path).replace('src/', '', 1))
    return sites


def main():
    profile_dir, binary, report_path = sys.argv[1:4]
    sources = sorted(glob.glob('src/branching/*.cpp'))
    raw = sorted(glob.glob(os.path.join(profile_dir, 'raw', '*.profraw')))
    tests = sorted({os.path.basename(p).split('@')[0] for p in raw})

    all_profile = os.path.join(profile_dir, 'all.profdata')
    merge(raw, all_profile)
    functions = export_functions(binary, all_profile, sources)
    names = demangle([f['name'] for f in functions])
    for function, name in zip(functions, names):
        function['display'] = name

    covering = defaultdict(set)
    for test in tests:
        test_profile = os.path.join(profile_dir, test + '.profdata')
        merge([p for p in raw if os.path.basename(p).startswith(test + '@')], test_profile)
        for function in export_functions(binary, test_profile, sources):
            if function['count'] > 0:
                covering[(function['name'], function['file'])].add(test)

    counts = {source: line_counts(binary, all_profile, source) for source in sources}
    rules = load_exclusions()

    def enclosing(path, line):
        best = None
        for function in functions:
            if function['file'] == path and function['start'] <= line <= function['end']:
                if best is None or function['end'] - function['start'] < best['end'] - best['start']:
                    best = function
        return best['display'] if best else ''

    # uncovered ranges: consecutive uncovered code lines, non-code lines in between allowed
    uncovered = []
    for source in sources:
        current = []
        for number in sorted(counts[source]):
            count, code = counts[source][number]
            if count is None:
                continue
            if count == 0:
                current.append((number, code))
                continue
            if current:
                uncovered.append((source, current))
                current = []
        if current:
            uncovered.append((source, current))

    failures = []
    explained = []
    uncovered_rows = []
    for source, block in uncovered:
        code_lines = [code.strip() for _, code in block]
        function = enclosing(source, block[0][0])
        reason = reason_for(source, function, code_lines, rules)
        if reason is None:
            failures.append('%s:%d has no reason' % (source, block[0][0]))
            reason = '**unexplained**'
        else:
            explained.append((source, block[0][0], block[-1][0]))
        location = '%s:%d' % (source.replace('src/branching/', ''), block[0][0])
        if len(block) > 1:
            location += '-%d' % block[-1][0]
        first = code_lines[0].replace('|', '\\|')
        if len(first) > 70:
            first = first[:67] + '...'
        uncovered_rows.append('| `%s` | `%s` | `%s` | %s |' % (location, function, first, reason))
    for rule in rules:
        if not rule['used']:
            failures.append('exclusion matches no uncovered code: %s | %s | %s' % (
                rule['path'], rule['function'], rule['snippet']))

    def is_explained(function):
        return any(path == function['file'] and lo <= function['start'] and function['end'] <= hi + 1
                   for path, lo, hi in explained)

    lines = ['# Branching test coverage', '']
    lines.append(
        'Generated by `scripts/branching/coverage.sh` on %s: %d test files in `test/sql/branch/`, each run on %s. '
        'Only `src/branching/` is instrumented; upstream files contain only calls into it (see '
        '[TOUCHPOINTS.md](TOUCHPOINTS.md)).' % (datetime.date.today().isoformat(), len(tests), ', '.join(CONFIGS))
    )
    lines += ['', '## Summary', '', '| File | Functions executed | Lines covered |', '| --- | --- | --- |']
    total_functions = total_executed = total_lines = total_covered = 0
    for source in sources:
        file_functions = [f for f in functions if f['file'] == source]
        executed = sum(1 for f in file_functions if f['count'] > 0)
        code = [c for c, _ in counts[source].values() if c is not None]
        covered = sum(1 for c in code if c)
        total_functions += len(file_functions)
        total_executed += executed
        total_lines += len(code)
        total_covered += covered
        lines.append(
            '| `%s` | %d / %d | %d / %d (%.1f%%) |'
            % (source, executed, len(file_functions), covered, len(code), 100.0 * covered / max(len(code), 1))
        )
    line_pct = 100.0 * total_covered / max(total_lines, 1)
    lines.append(
        '| **Total** | **%d / %d** | **%d / %d (%.1f%%)** |'
        % (total_executed, total_functions, total_covered, total_lines, line_pct)
    )
    unexplained_functions = [f for f in functions if f['count'] == 0 and not is_explained(f)]
    if unexplained_functions:
        failures.append('never executed: ' + ', '.join(f['display'] for f in unexplained_functions))
    if line_pct < LINE_TARGET:
        failures.append('line coverage %.1f%% is below %.0f%%' % (line_pct, LINE_TARGET))

    lines += ['', '## Upstream call sites', '']
    lines.append('Each `DuckLakeBranching::` function is what one upstream call site runs.')
    lines += ['', '| Call | Called from | Lines covered | Test files |', '| --- | --- | --- | --- |']
    sites = upstream_call_sites()
    for name in sorted(sites):
        matches = [f for f in functions if f['display'] == 'DuckLakeBranching::' + name]
        for function in matches:
            span = [
                counts[function['file']].get(n, (None, ''))[0]
                for n in range(function['start'], function['end'] + 1)
            ]
            code = [c for c in span if c is not None]
            tests_for = sorted(covering[(function['name'], function['file'])])
            lines.append(
                '| `%s` | %s | %d / %d | %d |'
                % (name, ', '.join('`%s`' % s for s in sorted(sites[name])), sum(code), len(code), len(tests_for))
            )
        if not matches:
            failures.append('call site %s has no function in src/branching' % name)

    lines += ['', '## Functions', '']
    lines.append('Every function in `src/branching/`, the share of its lines the tests execute, and the test files '
                 'that execute it.')
    for source in sources:
        lines += ['', '### `%s`' % source, '', '| Function | Lines covered | Test files |', '| --- | --- | --- |']
        for function in sorted((f for f in functions if f['file'] == source), key=lambda f: f['start']):
            span = [
                counts[source].get(n, (None, ''))[0] for n in range(function['start'], function['end'] + 1)
            ]
            code = [c for c in span if c is not None]
            tests_for = sorted(covering[(function['name'], function['file'])])
            if function['count'] == 0:
                test_text = 'not executed (see Uncovered lines)' if is_explained(function) else '**not executed**'
            elif len(tests_for) == len(tests):
                test_text = 'all %d' % len(tests)
            else:
                test_text = ', '.join(tests_for)
            lines.append(
                '| `%s` (line %d) | %d / %d | %s |' % (function['display'], function['start'], sum(code), len(code),
                                                       test_text)
            )

    lines += ['', '## Uncovered lines', '']
    lines.append('Code lines no test executes, with the reason. Reasons come from '
                 '`scripts/branching/coverage_exclusions.txt`; an `InternalException` is an invariant no SQL '
                 'statement can break.')
    lines += ['', '| Location | Function | Code | Reason |', '| --- | --- | --- | --- |'] + uncovered_rows

    if failures:
        lines += ['', '## Gaps', ''] + ['- ' + failure for failure in failures]
    open(report_path, 'w').write('\n'.join(lines) + '\n')
    print('functions %d/%d, lines %d/%d (%.1f%%), uncovered ranges %d' % (
        total_executed, total_functions, total_covered, total_lines, line_pct, len(uncovered)))
    for failure in failures:
        print('GAP: ' + failure)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
