#!/usr/bin/env python3

################################################################################
#
# NAME
# 
#        dtrsm-correctness.py - verifies the dtrsm example
#
# SYNOPSIS
# 
#        dtrsm-correctness.py [cores]
#
# DESCRIPTION
# 
#        Verifies the correctness of the dtrsm example (and indirectly
#        the runtime system).
#
#        Uses at most 4 cores unless otherwise specified on the
#        command line.
#
# NOTE
#
#        Must be run from the directory containing the "test-dtrsm.x"
#        executable.
#
################################################################################

import subprocess
import sys
import random

# Runs the "test-dtrsm.x" executable and returns True if the verification passed.
def run_test(matrix_size, nrhs, block_size, num_workers, reserved_set_size):
    cmd = './test-dtrsm.x {0} {1} {2} {3} {4}'.format(matrix_size, nrhs, block_size, num_workers, reserved_set_size)
    process = subprocess.run(args=cmd, shell=True, stdout=subprocess.PIPE, universal_newlines=True)
    out = process.stdout
    for line in out.splitlines():
        if line.startswith('Verifying the solution'):
            words = line.split()
            verified = (words[3] == 'PASSED')
    return verified

# Set the number of test cases per value of q. 
num_tests = 10

# Select the maximum number of cores to use.
p_max = 4
if len(sys.argv) == 2:
    p_max = int(sys.argv[1])

# Loop through values of q.    
for q in range(0, min(3, p_max - 1) + 1):
    if q == 0:
        print('Testing the REGULAR mode (q = 0)...')
    else:
        print('Testing the FIXED mode with q = {0:d}...'.format(q))

    # Initialize counters.
    num_passed = 0
    num_failed = 0

    # Loop through each test case.
    for test in range(num_tests):
        # Generate random inputs. 
        n = random.randint(500, 2000)
        m = random.randint(1, 500)
        b = random.randint(50, 500)
        p = random.randint(q + 1, p_max)
        print(' ... n = {0:4d}, m = {1:3d}, b = {2:3d}, p = {3:2d}, q = {4:1d} ... '.format(n, m, b, p, q), end='')

        # Run the program.
        verified = run_test(n, m, b, p, q)

        # Count the number of passed/failed tests.
        if verified:
            num_passed += 1
            print('PASSED')
        else:
            num_failed += 1
            print('FAILED')

    # Print summary.
    if num_failed == 0:
        print('PASSED')
    else:
        print('FAILED (passed {0:d} of {1:d} tests)'.format(num_passed, num_failed))
    print()
        
                
