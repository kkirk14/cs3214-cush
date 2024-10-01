#!/usr/bin/python
#
# Tests the functionality of cd implementation
# Also serves as example of how to write your own
# custom functionality tests.
#
import atexit, proc_check, time
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

#################################################################
# 
# Boilerplate ends here, now write your specific test.
#
#################################################################
# Step 1. Create a temporary directory structure
#
import tempfile, shutil, os
tmpdir = tempfile.mkdtemp("-cush-cd-tests")
subdir = os.path.join(tmpdir, "subdir")
os.mkdir(subdir)

# make sure it gets cleaned up if we exit
def cleanup():
    shutil.rmtree(tmpdir)

atexit.register(cleanup)

#################################################################
# Step 2. Run cd into the temporary directory
#
sendline("cd " + tmpdir)

expect_prompt("Shell did not print expected prompt (2) after cd into " + tmpdir)

#################################################################
# Step 3. Check pwd to confirm the current directory
#
sendline("pwd")
expect_exact(tmpdir, "pwd did not show the expected directory " + tmpdir)

#################################################################
# Step 4. Run cd into the subdir
#
sendline("cd subdir")
expect_prompt("Shell did not print expected prompt (3) after cd into subdir")

sendline("pwd")
expect_exact(subdir, "pwd did not show the expected subdir " + subdir)

#################################################################
# Step 5. Run cd .. to go back to the parent directory
#
sendline("cd ..")
expect_prompt("Shell did not print expected prompt (4) after cd ..")

sendline("pwd")
expect_exact(tmpdir, "pwd did not show the expected parent directory " + tmpdir + " after cd ..")

#################################################################
# Step 6. Test cd without arguments (should go to home)
#
sendline("cd")
expect_prompt("Shell did not print expected prompt (5) after cd to home")

sendline("pwd")
expected_home = os.getenv("HOME")
expect_exact(expected_home, "pwd did not show the expected home directory " + expected_home)

test_success()
