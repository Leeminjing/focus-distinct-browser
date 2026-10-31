"""Portable summaries; raw failures are kept separate from successful latencies."""
import csv
import math
import statistics


def distribution(values):
    values = sorted(values)
    return {'samples': len(values), 'median': statistics.median(values) if values else None,
            'p95': values[math.ceil(.95 * len(values)) - 1] if values else None,
            'min': values[0] if values else None, 'max': values[-1] if values else None}


def summarize(requests):
    result = {}
    for name in sorted({r['operation'] for r in requests}):
        rows = [r for r in requests if r['operation'] == name]
        good = [r['ms'] for r in rows if r['ok']]
        result[name] = {**distribution(good), 'attempts': len(rows),
                       'errors': sum(not r['ok'] for r in rows),
                       'error_rate': sum(not r['ok'] for r in rows) / len(rows)}
    return result


def resources(rows):
    result = {}
    for phase in sorted({r['phase'] for r in rows}):
        result[phase] = {}
        selected = [r for r in rows if r['phase'] == phase]
        for group in ('host', 'webview', 'client'):
            members = [r['groups'][group] for r in selected]
            result[phase][group] = {key: distribution([r[key] for r in members]) for key in
                ('processes', 'private_bytes', 'working_set_bytes', 'cpu_one_core_percent', 'cpu_capacity_percent')}
            result[phase][group]['memory_coverage_complete'] = all(r['processes'] == r['memory_readable'] for r in members)
    return result


def write_csv(path, rows, fields):
    with open(path, 'w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=fields, extrasaction='ignore')
        writer.writeheader(); writer.writerows(rows)
