#!/usr/bin/env python3

################################################################################
#
# NAME
# 
#        dpotrf-performance.py - runs performance tests on the dpotrf example
#
# SYNOPSIS
# 
#        dpotrf-performance.py config.json
#
# DESCRIPTION
# 
#        Runs performance tests on the dpotrf example. 
#
#        The parameters for the sweep are specified in an external
#        json configuration file (see CONFIGURATION below for
#        details). There is a sample configuration file in the
#        data/dpotrf/ directory of this repository. Modify at least
#        the range for p to match the machine you are using.
#
#        The sweep uses uniformly spaced ranges for each of n, b, p,
#        and q.
#
# CONFIGURATION
#
#        The configuration file uses the json format. A range for
#        parameter X (X = n, b, p, q) is specified by three keys:
#        X_min, X_max, and X_step.
#
#        Below is a complete example of a configuration file. Do not
#        add or remove any keys but change the values as you like.
#
#        {
#            "n_min" : 1000,
#            "n_max" : 6000,
#            "n_step" : 1000,
#            
#            "b_min" : 50,
#            "b_max" : 500,
#            "b_step" : 10,
#            
#            "p_min" : 7,
#            "p_max" : 28,
#            "p_step" : 7,
#            
#            "q_min" : 0,
#            "q_max" : 4,
#            "q_step" : 1
#        }
#
# OUTPUT
#
#        The program prints a table with the following columns:
#
#        n         - The matrix size.
#        b         - The tile size.
#        p         - The number of cores.
#        q         - The size of the reserved set.
#        time      - The parallel execution time.
#        crit-path - The length of the critical path (if q > 0).
#        long-path - The length of the longest path.
#
#        For example, the first few lines might look like this:
#
#           n     b  p  q       time  crit-path  long-path
#        1000    50  1  0   0.056440   0.000000   0.001653
#        1000    60  1  0   0.051537   0.000000   0.002055
#        1000    70  1  0   0.049782   0.000000   0.002453
#        1000    80  1  0   0.047021   0.000000   0.002974
#        1000    90  1  0   0.046890   0.000000   0.003597
#
# NOTE
#
#        Must be run from the directory containing the "test-dpotrf.x"
#        executable.
#
################################################################################

import subprocess
import sys
import json

# Runs ./test-dpotrf.x with specified inputs.
#
# The test is repeated a few times and the result from the fastest
# execution is returned.
#
# Returns a tuple (a, b, c) where a = time, b = critical path length,
# and c = longest path length.
def run_test(matrix_size, block_size, num_workers, reserved_set_size):
    reps = 3
    cmd = './test-dpotrf.x {0} {1} {2} {3} 0'.format(matrix_size, block_size, num_workers, reserved_set_size)
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

# Check command line options.
if len(sys.argv) != 2:
    print('Usage: {0:s} config.json'.format(sys.argv[0]))
    sys.exit()

# Read configuration file.    
with open(sys.argv[1]) as file:
    config = json.load(file)
    file.close()

# Generate lists of parameters based on configuration.
matrix_size_list       = list(range(config['n_min'], config['n_max'] + 1, config['n_step']))
num_workers_list       = list(range(config['p_min'], config['p_max'] + 1, config['p_step']))
reserved_set_size_list = list(range(config['q_min'], config['q_max'] + 1))
block_size_list        = list(range(config['b_min'], config['b_max'] + 1, config['b_step']))

# Print table header line. 
print('{0:>5s} {1:>5s} {2:>2s} {3:>2s} {4:>10s} {5:>10s} {6:>10s}'.format('n', 'b', 'p', 'q', 'time', 'crit-path', 'long-path'))

# Loop through all test cases.
for matrix_size in matrix_size_list:
    if matrix_size < 1:
        continue
    for num_workers in num_workers_list:
        if num_workers < 1:
            continue
        for reserved_set_size in reserved_set_size_list:
            if reserved_set_size >= num_workers:
                continue
            for block_size in block_size_list:
                result = run_test(matrix_size, block_size, num_workers, reserved_set_size)
                print('{0:5d} {1:5d} {2:2d} {3:2d} {4:10.6f} {5:10.6f} {6:10.6f}'.format(matrix_size, block_size, num_workers, reserved_set_size, result[0], result[1], result[2]))
                sys.stdout.flush()
                
