#!/usr/bin/env python3

#   libpulp - User-space Livepatching Library
#
#   Copyright (C) 2020 SUSE Software Solutions GmbH
#
#   This file is part of libpulp.
#
#   libpulp is free software; you can redistribute it and/or
#   modify it under the terms of the GNU Lesser General Public
#   License as published by the Free Software Foundation; either
#   version 2.1 of the License, or (at your option) any later version.
#
#   libpulp is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#   Lesser General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with libpulp.  If not, see <http://www.gnu.org/licenses/>.

from tests import *

# Start the test program and check default behavior
child = pexpect.spawn('./' + testname, timeout=1, env=preload,
                      encoding='utf-8')
child.logfile = sys.stdout

# Wait for the test program to be ready
child.expect('Waiting for input.\r\n')

# Apply live patch and check for new behavior
ret = subprocess.run([trigger, str(child.pid),
                      'libpagecross_livepatch1.ulp'])
if ret.returncode:
  print('Failed to apply livepatch #1 for libpagecross')
  exit(1)

child.sendline('')
index = child.expect(['default\r\n', 'patched\r\n']);
if index == 0:
  print('Patch not applied correctly')
  exit(1)

# Kill the child process and exit
child.close(force=True)
exit(0)
