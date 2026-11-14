# tests of source that also builds for the host, run without a GPU or Wine
test('host_geometry_budget', executable('host_geometry_budget', 'geometry_budget.cpp',
                                      include_directories : mullion_native_include_path,
                                      override_options : ['cpp_std=c++20'], native : true))
test('host_format', executable('host_format', 'format.cpp', include_directories : mullion_native_include_path,
                             override_options : ['cpp_std=c++20'], native : true))
# the session header's lines against the fixture SWTOR on Mac's report tests copy, and the switch registry
test('host_session', executable('host_session', 'session.cpp', override_options : ['cpp_std=c++20'], native : true),
     args : files('../swtor/fixtures/session.log'))
# the shader cache's key and tag follow its identity, the codegen id and the macOS build (src/mullion/mullion_cache_identity.hpp)
test('host_cache_identity', executable('host_cache_identity',
                                       ['cache_identity.cpp', '../../src/util/sha1/sha1_util.cpp', '../../src/util/sha1/sha1.c'],
                                       include_directories : include_directories('../../src/util'),
                                       override_options : ['cpp_std=c++20'], native : true))
# what a shader cache drops of other identities when it opens, and its count under the writers' lock
# (src/winemetal/unix/cache_prepare.h). linked as C++: the root meson.build gives native links their -arch for C++ only
test('host_cache_prepare', executable('host_cache_prepare', 'cache_prepare.c', link_args : ['-lsqlite3'],
                                      link_language : 'cpp', native : true))
# jdrly/mullion: MULLION_D3D12_GUARD_STATIC_RANGES in the native converter's output, static ranges against volatile
test('host_guard_static_ranges', executable('host_guard_static_ranges', 'guard_static_ranges.cpp',
                                            override_options : ['cpp_std=c++20'], native : true),
     args : [airconv_cli_darwin], timeout : 120)
# jdrly/mullion: the shader cache's key changes with the compile flags. its argument loop is CompileFunction's own,
# copied out of src/d3d12/d3d12_pipeline_graphics.cpp at build time
compile_key_arguments = custom_target('compile_key_arguments',
                                      input : '../../src/d3d12/d3d12_pipeline_graphics.cpp',
                                      output : 'compile_key_arguments.inc',
                                      command : [find_program('python3', native : true), files('compile_key.py'),
                                                 '@INPUT@', '@OUTPUT@'])
test('host_compile_key', executable('host_compile_key',
                                    ['compile_key.cpp', compile_key_arguments, '../../src/util/sha1/sha1_util.cpp',
                                     '../../src/util/sha1/sha1.c'],
                                    include_directories : include_directories('../../src/util'),
                                    override_options : ['cpp_std=c++20'], native : true))
# the converter's codegen id (tools/codegen_id.py)
test('host_codegen_id', find_program('python3', native : true),
     args : files('codegen_id.py') + [meson.project_source_root() / 'tools/codegen_id.py'])
# the bridge's layer setters with the main thread held (MULLION_LAYER_WAIT): a layer without a device, no Wine
test('host_layer_apply', executable('host_layer_apply', ['layer_apply.c', '../../src/winemetal/unix/layer.c'],
                                    include_directories : include_directories('../../src/winemetal/unix'),
                                    c_args : ['-ObjC'],
                                    # a C program: a cross build gives native links -arch for C++ only (meson.build)
                                    link_args : ['-framework', 'Foundation', '-framework', 'QuartzCore',
                                                 '-framework', 'CoreGraphics', '-framework', 'Metal']
                                                + (mullion_crossbuild ? ['-arch', native_compiler_arch] : []),
                                    native : true))
# the STATS line's arithmetic and format, and the fixture's STATS lines
test('host_stats', executable('host_stats', 'stats.cpp', override_options : ['cpp_std=c++20'], native : true),
     args : files('../swtor/fixtures/session.log'))
# the line a command buffer the GPU failed writes, against the same fixture, and its limit of 30 lines in 10 s
test('host_gpu_error', executable('host_gpu_error', 'gpu_error.cpp', override_options : ['cpp_std=c++20'], native : true),
     args : files('../swtor/fixtures/session.log'))
# the HITCH and COMPILE lines, their scopes, the MULLION-PIPELINE line and its limiter, and the fixture's lines
test('host_hitch', executable('host_hitch', 'hitch.cpp', override_options : ['cpp_std=c++20'], native : true),
     args : files('../swtor/fixtures/session.log'))
# the lines a removed device, a command buffer that hasn't completed and a refused feature level write, against the
# same fixture
test('host_device_lines', executable('host_device_lines', 'device_lines.cpp', override_options : ['cpp_std=c++20'],
                                     native : true),
     args : files('../swtor/fixtures/session.log'))
# the DIAG MEM and MULLION-MEMORY-PRESSURE lines, MULLION_MEMSTATS's grammar, and the fixture's DIAG MEM lines
test('host_memory', executable('host_memory', 'memory.cpp', override_options : ['cpp_std=c++20'], native : true),
     args : files('../swtor/fixtures/session.log'))
test('host_rc_self_move', executable('host_rc_self_move', 'rc.cpp',
                                    override_options : ['cpp_std=c++20'], native : true))
test('host_denorm', executable('host_denorm', 'denorm.cpp', include_directories : include_directories('metal'),
                             override_options : ['cpp_std=c++20'], native : true))
test('host_tessellator',
     executable('host_tessellator', ['tessellator.cpp', '../reference/reference.cpp'],
                include_directories : include_directories('metal', '../../src/airconv', '../../src/airconv/shaders', '../../libs'),
                # the reference's and the Metal source's, neither written for this compiler's warnings
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     timeout : 300)
# which DXIL operations shaders use, for tests/coverage.py; it reads DXIL with the LLVM the converter is built with
executable('dxilops', ['dxilops.cpp'], include_directories : llvm_include_path_darwin,
           link_args : [ llvm_ld_flags_darwin, llvm_deps ], cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true)
# the tests' own container hash against the hashes real shaders carry; it wants directories of them (MULLION_HASHED)
test('host_dxbc_hash', executable('host_dxbc_hash', ['dxbchash.cpp'], override_options : ['cpp_std=c++20'], native : true))
test('host_diag_profile', find_program('python3', native : true), args : files('diag.py'))
test('host_mutate', find_program('python3', native : true), args : files('mutate.py'))
test('host_install', find_program('python3', native : true),
     args : files('install.py') + [meson.project_source_root() / 'install.sh'])
test('host_lock', find_program('python3', native : true), args : files('lock.py') + [meson.project_source_root() / 'tests/run.py'])
test('host_admission', find_program('python3', native : true), args : files('admission.py') + [meson.project_source_root() / 'tests/run.py'])
test('host_frames', find_program('python3', native : true),
     args : files('frames.py') + [meson.project_source_root() / 'tools/frames.py'])
