#!/usr/bin/python
#
# Tests the functionality of history implementation
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
# Step 1. Execute a few commands
#
sendline("echo hello")
expect_exact("hello", "echo hello did not work as expected")
expect_prompt("Shell did not print expected prompt after echo hello")

sendline("pwd")
expect_prompt("Shell did not print expected prompt after pwd")

sendline("ls")
expect_prompt("Shell did not print expected prompt after ls")

#################################################################
# Step 2. Run the history command
#
sendline("history")

# Check if the commands are listed in the history output
expected_history = [
    "echo hello",
    "pwd",
    "ls",
    "history"  # The `history` command itself will also appear in the output
]

for cmd in expected_history:
    expect_exact(cmd, "history did not include the expected command: " + cmd)

#################################################################
# Step 3. Test shorthand commands that use history to function
#
sendline("!!")

#Copies the last command (history)
expected_recent_history = [
    "echo hello",
    "pwd",
    "ls",
    "history",
    "history"
]

for cmd in expected_recent_history:
    expect_exact(cmd, "!! did not show the expected command: " + cmd)

sendline("!echo")

expect_exact("hello", "!echo did not work as expected")

sendline("!-1")

expect_exact("hello", "!-1 did not work as expected")

sendline("!5")

#Copies the 5th command (history)
expected_recent_history = [
    "echo hello",
    "pwd",
    "ls",
    "history",
    "history",
    "echo hello",
    "echo hello",
    "history"
]

for cmd in expected_recent_history:
    expect_exact(cmd, "!5 did not show the expected command: " + cmd)
#################################################################
# Test success
#
test_success()
