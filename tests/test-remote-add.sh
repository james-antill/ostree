#!/bin/bash
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e

. $(dirname $0)/libtest.sh

echo '1..3'

setup_test_repository "bare"
$OSTREE remote add origin http://example.com/ostree/gnome
echo "ok remote add"
assert_file_has_content $test_tmpdir/repo/config "example.com"
echo "ok config"

$OSTREE remote add --no-gpg-verify another http://another.com/repo
assert_file_has_content $test_tmpdir/repo/config "gpg-verify=false"
echo "ok remote no gpg-verify"

$OSTREE remote delete another
echo "ok remote delete"

if $OSTREE remote delete nosuchremote 2>err.txt; then
    assert_not_reached "Deleting remote unexpectedly succeeded"
fi
assert_file_has_content err.txt "error: "


