#!/bin/sh
# test_import.sh -- the other direction, an import: `9share serve` runs ssh (a stand-in that runs `9share offer` at once), offer starts daud
# there (daud --dial --once, its carrier `9share carry`), and the link carries sixteen megabytes each way while the stand-in ssh is killed
# again and again, then a relay that does not know daud's session.  Run without a secret (ssh is trusted) and with one (SCRAM-SHA-256).
dir=${NINESHARE_TESTDIR:-${0%/*}}
NINESHARE_TEST_IMPORT=1 "$dir/test_resume" || exit 1
NINESHARE_TEST_IMPORT=1 NINESHARE_TEST_SECRET=1 "$dir/test_resume" || exit 1
