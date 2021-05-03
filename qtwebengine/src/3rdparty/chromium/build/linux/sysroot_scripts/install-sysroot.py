#!/usr/bin/env python
# Copyright (c) 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Install Debian Wheezy sysroots for building chromium.
"""

# The sysroot is needed to ensure that binaries will run on Debian Wheezy,
# the oldest supported linux distribution.  This script can be run manually but
# is more often run as part of gclient hooks. When run from hooks this script
# in a no-op on non-linux platforms.

# The sysroot image could be constructed from scratch based on the current
# state or Debian Wheezy but for consistency we currently use a pre-built root
# image. The image will normally need to be rebuilt every time chrome's build
# dependencies are changed.

import hashlib
import platform
import optparse
import os
import re
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.append(os.path.dirname(os.path.dirname(SCRIPT_DIR)))
import detect_host_arch
import gyp_chromium
import gyp_environment


URL_PREFIX = 'http://commondatastorage.googleapis.com'
URL_PATH = 'chrome-linux-sysroot/toolchain'
REVISION_AMD64 = '81463d905d9f7aa0153bae9f703728632ce8b0f6'
REVISION_ARM = '81463d905d9f7aa0153bae9f703728632ce8b0f6'
REVISION_I386 = '81463d905d9f7aa0153bae9f703728632ce8b0f6'
REVISION_MIPS = '81463d905d9f7aa0153bae9f703728632ce8b0f6'
TARBALL_AMD64 = 'debian_wheezy_amd64_sysroot.tgz'
TARBALL_ARM = 'debian_wheezy_arm_sysroot.tgz'
TARBALL_I386 = 'debian_wheezy_i386_sysroot.tgz'
TARBALL_MIPS = 'debian_wheezy_mips_sysroot.tgz'
TARBALL_AMD64_SHA1SUM = 'a5a2483123f6bd63f3f029996451426168606697'
TARBALL_ARM_SHA1SUM = 'fd70dfa1bde44142b17e5b400be3b9ce6625bf2e'
TARBALL_I386_SHA1SUM = 'bc8d70311edcbdce8f70af779333dc35ca139777'
TARBALL_MIPS_SHA1SUM = 'ac60722b79bce906768911192329607393090c4a'
SYSROOT_DIR_AMD64 = 'debian_wheezy_amd64-sysroot'
SYSROOT_DIR_ARM = 'debian_wheezy_arm-sysroot'
SYSROOT_DIR_I386 = 'debian_wheezy_i386-sysroot'
SYSROOT_DIR_MIPS = 'debian_wheezy_mips-sysroot'

valid_archs = ('arm', 'i386', 'amd64', 'mips')


class Error(Exception):
  pass


def GetSha1(filename):
  sha1 = hashlib.sha1()
  with open(filename, 'rb') as f:
    while True:
      # Read in 1mb chunks, so it doesn't all have to be loaded into memory.
      chunk = f.read(1024*1024)
      if not chunk:
        break
      sha1.update(chunk)
  return sha1.hexdigest()


def DetectHostArch():
  # Figure out host arch using build/detect_host_arch.py and
  # set target_arch to host arch
  detected_host_arch = detect_host_arch.HostArch()
  if detected_host_arch == 'x64':
    return 'amd64'
  elif detected_host_arch == 'ia32':
    return 'i386'
  elif detected_host_arch == 'arm':
    return 'arm'
  elif detected_host_arch == 'mips':
    return 'mips'

  raise Error('Unrecognized host arch: %s' % detected_host_arch)


def DetectTargetArch():
  """Attempt for determine target architecture.

  This works by looking for target_arch in GYP_DEFINES.
  """
  # TODO(agrieve): Make this script not depend on GYP_DEFINES so that it works
  #     with GN as well.
  gyp_environment.SetEnvironment()
  supplemental_includes = gyp_chromium.GetSupplementalFiles()
  gyp_defines = gyp_chromium.GetGypVars(supplemental_includes)
  target_arch = gyp_defines.get('target_arch')
  if target_arch == 'x64':
    return 'amd64'
  elif target_arch == 'ia32':
    return 'i386'
  elif target_arch == 'arm':
    return 'arm'
  elif target_arch == 'arm64':
    return 'arm64'
  elif target_arch == 'mipsel':
    return 'mips'
  elif target_arch:
    raise Error('Unrecognized target_arch: %s' % target_arch)

  return None


def InstallDefaultSysroots():
  """Install the default set of sysroot images.

  This includes at least the sysroot for host architecture, and the 32-bit
  sysroot for building the v8 snapshot image.  It can also include the cross
  compile sysroot for ARM/MIPS if cross compiling environment can be detected.
  """
  host_arch = DetectHostArch()
  InstallSysroot(host_arch)

  if host_arch == 'amd64':
    InstallSysroot('i386')

  # Finally, if we can detect a non-standard target_arch such as ARM or
  # MIPS, then install the sysroot too.
  # Don't attampt to install arm64 since this is currently and android-only
  # architecture.
  target_arch = DetectTargetArch()
  if target_arch and target_arch not in (host_arch, 'i386', 'arm64'):
    InstallSysroot(target_arch)


def main():
  if options.running_as_hook and not sys.platform.startswith('linux'):
    return 0

  if options.running_as_hook:
    InstallDefaultSysroots()
  else:
    if not options.arch:
      print 'You much specify either --arch or --running-as-hook'
      return 1
    InstallSysroot(options.arch)

  return 0


def InstallSysroot(target_arch):
  # The sysroot directory should match the one specified in build/common.gypi.
  # TODO(thestig) Consider putting this else where to avoid having to recreate
  # it on every build.
  linux_dir = os.path.dirname(SCRIPT_DIR)
  if target_arch == 'amd64':
    sysroot = os.path.join(linux_dir, SYSROOT_DIR_AMD64)
    tarball_filename = TARBALL_AMD64
    tarball_sha1sum = TARBALL_AMD64_SHA1SUM
    revision = REVISION_AMD64
  elif target_arch == 'arm':
    sysroot = os.path.join(linux_dir, SYSROOT_DIR_ARM)
    tarball_filename = TARBALL_ARM
    tarball_sha1sum = TARBALL_ARM_SHA1SUM
    revision = REVISION_ARM
  elif target_arch == 'i386':
    sysroot = os.path.join(linux_dir, SYSROOT_DIR_I386)
    tarball_filename = TARBALL_I386
    tarball_sha1sum = TARBALL_I386_SHA1SUM
    revision = REVISION_I386
  elif target_arch == 'mips':
    sysroot = os.path.join(linux_dir, SYSROOT_DIR_MIPS)
    tarball_filename = TARBALL_MIPS
    tarball_sha1sum = TARBALL_MIPS_SHA1SUM
    revision = REVISION_MIPS
  else:
    raise Error('Unknown architecture: %s' % target_arch)

  url = '%s/%s/%s/%s' % (URL_PREFIX, URL_PATH, revision, tarball_filename)

  stamp = os.path.join(sysroot, '.stamp')
  if os.path.exists(stamp):
    with open(stamp) as s:
      if s.read() == url:
        print 'Debian Wheezy %s root image already up-to-date: %s' % \
            (target_arch, sysroot)
        return

  print 'Installing Debian Wheezy %s root image: %s' % (target_arch, sysroot)
  if os.path.isdir(sysroot):
    shutil.rmtree(sysroot)
  os.mkdir(sysroot)
  tarball = os.path.join(sysroot, tarball_filename)
  print 'Downloading %s' % url
  sys.stdout.flush()
  sys.stderr.flush()
  subprocess.check_call(['curl', '--fail', '-L', url, '-o', tarball])
  sha1sum = GetSha1(tarball)
  if sha1sum != tarball_sha1sum:
    raise Error('Tarball sha1sum is wrong.'
                'Expected %s, actual: %s' % (tarball_sha1sum, sha1sum))
  subprocess.check_call(['tar', 'xf', tarball, '-C', sysroot])
  os.remove(tarball)

  with open(stamp, 'w') as s:
    s.write(url)


if __name__ == '__main__':
  parser = optparse.OptionParser('usage: %prog [OPTIONS]', description=__doc__)
  parser.add_option('--running-as-hook', action='store_true',
                    default=False, help='Used when running from gclient hooks.'
                                        ' Installs default sysroot images.')
  parser.add_option('--arch', type='choice', choices=valid_archs,
                    help='Sysroot architecture: %s' % ', '.join(valid_archs))
  options, _ = parser.parse_args()
  sys.exit(main())
