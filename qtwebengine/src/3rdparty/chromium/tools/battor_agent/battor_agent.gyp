# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

{
  'targets': [
    {
      'target_name': 'battor_agent',
      'type': 'executable',
      'include_dirs': [
        '../..',
      ],
      'dependencies': [
        'battor_agent_lib',
        '../../device/serial/serial.gyp:device_serial',
        '../../device/serial/serial.gyp:device_serial_mojo',
        '../../third_party/mojo/mojo_public.gyp:mojo_environment_standalone',
        '../../third_party/mojo/mojo_public.gyp:mojo_public',
      ],
      'sources': [
        'battor_agent_bin.cc',
      ],
    },
    {
      'target_name': 'battor_agent_lib',
      'type': 'static_library',
      'include_dirs': [
        '../..',
      ],
      'sources': [
        'battor_agent.cc',
        'battor_agent.h',
        'battor_connection.cc',
        'battor_connection.h',
        'battor_connection_impl.cc',
        'battor_connection_impl.h',
        'battor_error.h',
        'battor_sample_converter.cc',
        'battor_sample_converter.h',
      ],
      'dependencies': [
        '../../base/base.gyp:base',
        '../../device/serial/serial.gyp:device_serial',
        '../../device/serial/serial.gyp:device_serial_mojo',
      ]
    },
    {
      'target_name': 'battor_agent_unittests',
      'type': '<(gtest_target_type)',
      'dependencies': [
        'battor_agent_lib',
        '../../base/base.gyp:base',
        '../../base/base.gyp:run_all_unittests',
        '../../base/base.gyp:test_support_base',
        '../../device/serial/serial.gyp:device_serial',
        '../../device/serial/serial.gyp:device_serial_test_util',        
	'../../testing/gmock.gyp:gmock',
        '../../testing/gtest.gyp:gtest',
        '../../third_party/mojo/mojo_public.gyp:mojo_environment_standalone',
        '../../third_party/mojo/mojo_public.gyp:mojo_public',
      ],
      'sources': [
	'battor_agent_unittest.cc',
        'battor_connection_impl_unittest.cc',
        'battor_protocol_types_unittest.cc',
        'battor_sample_converter_unittest.cc',
      ],
    },
  ],
}
