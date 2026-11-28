#!/bin/sh
# test_resume_scram.sh -- test_resume again, with a secret file at both ends, so the link authenticates (SCRAM-SHA-256) on every
# carrier: daud reads the secret from a file, the relay from the user's home.
NINESHARE_TEST_SECRET=1 exec "${NINESHARE_TESTDIR:-${0%/*}}/test_resume"
