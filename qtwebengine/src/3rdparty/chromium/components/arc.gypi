# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

{
  'targets': [
    {
      # GN version: //components/arc
      'target_name': 'arc',
      'type': 'static_library',
      'include_dirs': [
        '..',
      ],
      'dependencies': [
        'arc_mojo_bindings',
        '../base/base.gyp:base',
        '../chromeos/chromeos.gyp:chromeos',
        '../chromeos/chromeos.gyp:power_manager_proto',
        '../ipc/ipc.gyp:ipc',
        '../ui/arc/arc.gyp:arc',
        '../ui/aura/aura.gyp:aura',
        '../ui/base/ime/ui_base_ime.gyp:ui_base_ime',
        '../ui/base/ui_base.gyp:ui_base',
        '../ui/events/events.gyp:events_base',
      ],
      'sources': [
        'arc/auth/arc_auth_service.h',
        'arc/arc_bridge_bootstrap.cc',
        'arc/arc_bridge_bootstrap.h',
        'arc/arc_bridge_service.cc',
        'arc/arc_bridge_service.h',
        'arc/arc_bridge_service_impl.cc',
        'arc/arc_bridge_service_impl.h',
        'arc/arc_service_manager.cc',
        'arc/arc_service_manager.h',
        'arc/clipboard/arc_clipboard_bridge.cc',
        'arc/clipboard/arc_clipboard_bridge.h',
        'arc/ime/arc_ime_bridge.cc',
        'arc/ime/arc_ime_bridge.h',
        'arc/ime/arc_ime_ipc_host.cc',
        'arc/ime/arc_ime_ipc_host.h',
        'arc/input/arc_input_bridge.h',
        'arc/input/arc_input_bridge_impl.cc',
        'arc/input/arc_input_bridge_impl.h',
        'arc/settings/arc_settings_bridge.cc',
        'arc/settings/arc_settings_bridge.h',
        'arc/power/arc_power_bridge.cc',
        'arc/power/arc_power_bridge.h',
        'arc/video/arc_video_bridge.cc',
        'arc/video/arc_video_bridge.h',
        'arc/video/video_host_delegate.h',
      ],
    },
    {
      # GN version: //components/arc_test_support
      'target_name': 'arc_test_support',
      'type': 'static_library',
      'include_dirs': [
        '..',
      ],
      'dependencies': [
        '../base/base.gyp:base',
        'arc',
        'arc_mojo_bindings',
      ],
      'sources': [
        'arc/test/fake_app_instance.cc',
        'arc/test/fake_app_instance.h',
        'arc/test/fake_arc_bridge_instance.cc',
        'arc/test/fake_arc_bridge_instance.h',
        'arc/test/fake_arc_bridge_service.cc',
        'arc/test/fake_arc_bridge_service.h',
      ],
    },
    {
      # GN version: //components/arc:mojo_bindings
      'target_name': 'arc_mojo_bindings',
      'type': 'static_library',
      'includes': [
        '../third_party/mojo/mojom_bindings_generator.gypi',
      ],
      'sources': [
        'arc/common/app.mojom',
        'arc/common/arc_bridge.mojom',
        'arc/common/auth.mojom',
        'arc/common/clipboard.mojom',
        'arc/common/ime.mojom',
        'arc/common/input.mojom',
        'arc/common/notifications.mojom',
        'arc/common/power.mojom',
        'arc/common/process.mojom',
        'arc/common/settings.mojom',
        'arc/common/video.mojom',
      ],
    },
  ],
}
