#!/usr/bin/env python3

import subprocess
import sys
import json


# Runs ./test-dtrsm.x with specified inputs.
#
# The test is repeated a few times and the result from the fastest
# execution is returned.
#
# Returns a tuple (a, b, c) where a = time, b = critical path length, and c = longest path length.
def run_test(matrix_size, nrhs, block_size, num_workers, reserved_set_size):
    reps = 5
    cmd = './test-dtrsm.x {0} {1} {2} {3} {4} 0'.format(matrix_size, nrhs, block_size, num_workers, reserved_set_size)
    for rep in range(reps):
        process = subprocess.run(args=cmd, shell=True, stdout=subprocess.PIPE, universal_newlines=True)
        out = process.stdout
        for line in out.splitlines():
            if line.startswith('Execution time'):
                words = line.split()
                time = float(words[3])
            if line.startswith('Critical path length'):
                words = line.split();
                crit_path = float(words[4])
            if line.startswith('Longest path length'):
                words = line.split();
                long_path = float(words[4])
        result = (time, crit_path, long_path)
        if rep == 0 or result[0] < best_result[0]:
            best_result = result
    return best_result


if len(sys.argv) != 2:
    print('Usage: {0:s} config-file'.format(sys.argv[0]))
    sys.exit()
    
with open(sys.argv[1]) as file:
    config = json.load(file)
    file.close()

matrix_size_list       = list(range(config['n_min'], config['n_max'] + 1, config['n_step']))
nrhs                   = config['m']
num_workers_list       = list(range(config['p_min'], config['p_max'] + 1, config['p_step']))
reserved_set_size_list = list(range(config['q_min'], config['q_max'] + 1))
block_size_list        = list(range(config['b_min'], config['b_max'] + 1, config['b_step']))

print('{0:>5s} {1:>5s} {2:>2s} {3:>2s} {4:>10s} {5:>10s} {6:>10s}'.format('n', 'b', 'p', 'q', 'time', 'crit-path', 'long-path'))
for matrix_size in matrix_size_list:
    for num_workers in num_workers_list:
        for reserved_set_size in reserved_set_size_list:
            if reserved_set_size >= num_workers:
                continue
            for block_size in block_size_list:
                result = run_test(matrix_size, nrhs, block_size, num_workers, reserved_set_size)
                print('{0:5d} {1:5d} {2:2d} {3:2d} {4:10.6f} {5:10.6f} {6:10.6f}'.format(matrix_size, block_size, num_workers, reserved_set_size, result[0], result[1], result[2]))
                sys.stdout.flush()
                
