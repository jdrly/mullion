mullion_src = [
  'mullion_format.cpp',
  'mullion_names.cpp',
  'mullion_command_queue.cpp',
  'mullion_command.cpp',
  'mullion_capture.cpp',
  'mullion_info.cpp',
  'mullion_device.cpp',
  'mullion_buffer.cpp',
  'mullion_texture.cpp',
  'mullion_context.cpp',
  'mullion_dynamic.cpp',
  'mullion_staging.cpp',
  'mullion_hud_state.cpp',
  'mullion_allocation.cpp',
  'mullion_presenter.cpp',
  'mullion_sampler.cpp',
  'mullion_resource_initializer.cpp',
  'mullion_shader_cache.cpp',
  'mullion_scaler.cpp',
  'mullion_subresource.cpp',
  'mullion_deptrack.cpp',
  'mullion_counter.cpp',
  'mullion_fence.cpp',
  'mullion_keyed_mutex.cpp',
  'mullion_session.cpp',
]

mullion_shaders = [
  'mullion_command.metal'
]

# Metal 3.2, macOS 15's, and the oldest macOS the driver runs on (meson.build), not whatever the Xcode that builds it
# defaults to: the driver aborts when Metal refuses this library. 3.1, the converter's helper shaders' language,
# rejects this file's lambdas
mullion_shaders_ir = metalir_generator.process(
  mullion_shaders,
  extra_args: ['-std=metal3.2', '--target=air64-apple-macos15.0'],
)

mullion_shaders_lib = metallib_generator.process(mullion_shaders_ir)

mullion_shaders_header = hexdump_generator.process(mullion_shaders_lib)

mullion_lib = static_library('mullion', mullion_src, mullion_shaders_header, mullion_version, mullion_codegen,
  dependencies        : [ util_dep, airconv_forward_dep, winemetal_dep ],
  include_directories : [ mullion_include_path ],
)

mullion_dep = declare_dependency(
  link_with           : [ mullion_lib ],
  include_directories : [ mullion_include_path, include_directories('.') ],
)
