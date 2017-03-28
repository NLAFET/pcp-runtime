#!/usr/bin/env python3

################################################################################
#
# NAME
#
#        dpotrf-analyze-performance.py - analyzes performance data
#
# SYNOPSIS
#
#        dpotrf-analyze-performance.py < data
#
# DESCRIPTION
#
#        Analyzes the data generated by the dpotrf-performance.py
#        script.
#
#        The result is a table that shows for each pair of (n, p) the
#        following information:
#
#        s (x/y:z)
#
#        where
#
#        s - The speedup of the FIXED mode over the REGULAR mode with
#            optimal parameters for both.
#        x - The optimal tile size for the REGULAR mode.
#        y - The optimal tile size for the FIXED mode.
#        z - The optimal size of the reserved set for the FIXED mode.
#
# EXAMPLE
#
#        The results/kebnekaise/dpotrf/data file was generated on the
#        Kebnekaise system at HPC2N. To run the analysis on this data,
#        run (from the top-level directory):
#
#        test/dpotrf/dpotrf-analyze-performance.py < results/kebnekaise/dpotrf/data
#
#        This will generate the following output:
#
#                |              p= 7 |              p=14 |              p=21 |              p=28 | 
#        --------+-------------------+-------------------+-------------------+-------------------+
#        n= 1000 | 0.990 (110/120:1) | 1.071 ( 90/ 70:1) | 1.129 (130/ 60:1) | 1.026 ( 90/100:1) |
#        n= 2000 | 0.910 (120/200:1) | 1.003 (120/120:1) | 1.022 (120/120:1) | 1.022 (140/120:1) |
#        n= 3000 | 0.899 (200/240:1) | 0.971 (160/160:1) | 0.988 (160/160:1) | 0.986 (160/160:2) |
#        n= 4000 | 0.897 (360/380:1) | 0.952 (240/240:1) | 0.995 (160/200:1) | 0.997 (200/200:1) |
#        n= 5000 | 0.867 (400/460:1) | 0.954 (240/240:1) | 0.983 (240/240:1) | 0.986 (240/240:1) |
#        n= 6000 | 0.856 (440/440:1) | 0.956 (320/320:1) | 0.965 (320/320:1) | 0.980 (240/240:1) |
#
#        Note that while there are some cells with a speedup > 1,
#        these all correspond to an optimal reserved set size of 1,
#        which means that the speedup is actually not due to the
#        effect of parallelizing the critical path.
#
# NOTE
#
#        Must be run from the directory containing the "test-dpotrf.x"
#        executable.
#
################################################################################

import sys

# Skip header line. 
sys.stdin.readline()

# Read records.
data = []
for line in sys.stdin:
    line = line.strip()
    fields = line.split()
    record = {'n' : int(fields[0]),
              'b' : int(fields[1]),
              'p' : int(fields[2]),
              'q' : int(fields[3]),
              'time' : float(fields[4]),
              'crit-path' : float(fields[5]),
              'long-path' : float(fields[6])}
    data.append(record)

# Find set of n.
n_set = set([r['n'] for r in data])

# Find set of p.
p_set = set([r['p'] for r in data])

# Create table.
table = []
for x in range(len(n_set)):
    row = []
    for y in range(len(p_set)):
        row.append((None, None, None))
    table.append(row)

# For each n.
for row, n in enumerate(sorted(n_set)):
    # For each p.
    for col, p in enumerate(sorted(p_set)):
        # Find q = 0 data.
        subset_q0 = [r for r in data if r['n'] == n and r['p'] == p and r['q'] == 0]

        # Find optimal b.
        best_q0 = min(subset_q0, key=lambda r:r['time'])

        # Find q > 0 data.
        subset_qpos = [r for r in data if r['n'] == n and r['p'] == p and r['q'] > 0]

        # Find optimal q and b.
        best_qpos = min(subset_qpos, key=lambda r:r['time'])

        # Compute speedup.
        numerator   = best_q0['time']
        denominator = best_qpos['time']
        speedup     = numerator / denominator

        # Store in table.
        table[row][col] = (best_q0, best_qpos, speedup)

# Print table.
num_cols = len(p_set)
print('        | ', end='')
for col, p in enumerate(sorted(p_set)):
    print('             p={0:2d} | '.format(p), end='')
print()
print('--------+', end='')
for x in range(len(p_set)):
    print('-------------------+', end='')
print()
for row, n in enumerate(sorted(n_set)):
    print('n={0:5d} | '.format(n), end='')
    line = []
    for col, p in enumerate(sorted(p_set)):
        best_q0, best_qpos, speedup = table[row][col]
        b0   = best_q0['b']
        bpos = best_qpos['b']
        qpos = best_qpos['q']
        line.append('{0:5.3f} ({1:3d}/{2:3d}:{3:1d})'.format(speedup, b0, bpos, qpos))
    print(' | '.join(line) + ' |')

