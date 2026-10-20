"""Merge listing runs into one file per name; mark gaps and conflicting lines."""
import pickle, sys, os
from collections import Counter
runs = pickle.load(open(sys.argv[1], 'rb'))
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)
report = []
for name, rs in sorted(runs.items()):
    votes = {}
    for r in rs:
        for ln, t in r['lines'].items():
            votes.setdefault(ln, Counter())[t.rstrip()] += 1
    mx = max(votes)
    out, gaps, conflicts = [], 0, []
    for ln in range(1, mx + 1):
        if ln not in votes:
            out.append(f'/* @@ line {ln} missing */'); gaps += 1; continue
        (best, n), *rest = votes[ln].most_common()
        out.append(best)
        if rest:
            conflicts.append((ln, votes[ln].most_common()))
    open(os.path.join(outdir, name), 'w').write('\n'.join(out) + '\n')
    report.append(f'{name:18} lines={mx:5} missing={gaps:4} conflicts={len(conflicts)}')
    if conflicts:
        with open(os.path.join(outdir, name + '.conflicts.txt'), 'w') as f:
            for ln, c in conflicts:
                f.write(f'--- line {ln}\n' + ''.join(f'  [{n}x] {t}\n' for t, n in c))
open(os.path.join(outdir, '_report.txt'), 'w').write('\n'.join(report) + '\n')
print('\n'.join(report))
