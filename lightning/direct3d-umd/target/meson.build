# Copyright © 2021 VMware, Inc
# Copyright © 2018 Intel Corporation
# SPDX-License-Identifier: MIT

libgallium_d3d10_def = custom_target(
  'd3d10.def',
  input: 'd3d10.def.in',
  output : 'd3d10.def',
  command : gen_vs_module_defs_normal_command,
)

gallium_d3d10_name = get_option('gallium-d3d10-dll-name')
libgallium_d3d10 = shared_library(
  gallium_d3d10_name,
  ['d3d10_gdi.c', 'd3d10_staging.c', 'd3d10_buffer_rtv.c', 'd3d10_context.c', 'd3d10_device_loss.c', 'd3d10_indexed_indirect.c', 'd3d10_rasterized_stream.c', 'd3d10_forced_samples.c', 'd3d10_resource_hazard.c', 'd3d10_rotated_blit.c', zink_device_info[0], zink_instance[0]],
  vs_module_defs : libgallium_d3d10_def,
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_d3d10umd, inc_gallium_winsys, inc_gallium_winsys_sw, inc_gallium_drivers, inc_winddk,
    inc_vulkan_util, include_directories('../../drivers/zink')
  ],
  link_whole : [libd3d10umd],
  # Rust owns native command recording allocation, snapshots and list lifetime.
  link_args : [join_paths(meson.project_source_root(),
                         'src/virtio/vulkan/lightning/windows-transport.lib')],
  link_with : [
    libgallium, libwsgdi
  ],
  dependencies : [
    dep_ws2_32, idep_nir, driver_zink, idep_xmlconfig, idep_mesautil,
    idep_vulkan_util_headers
  ],
  name_prefix: '',
  install : true,
)

if with_tests
  test(
    'd3d10',
    executable(
      'test_d3d10',
      files('tests/tri.cpp'),
      cpp_args : [cpp_msvc_compat_args],
      dependencies : [cpp.find_library('d3d11')],
      link_with : [libgallium_d3d10],
    ),
    suite : ['d3d10'],
  )
endif
