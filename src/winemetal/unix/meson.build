winemetal_unix_src = [
  'winemetal_unix.c',
  'cache.c',
  'layer.c',
  'window.c',
]

if wine_build_path != ''
  winemac_so = join_paths(wine_build_path, 'dlls/winemac.drv/winemac.so')
  ntdll_so = join_paths(wine_build_path, 'dlls/ntdll/ntdll.so')
elif wine_install_path != ''
  winemac_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'winemac.so')
  ntdll_so = join_paths(wine_install_path, 'lib/wine', unix_install_dir, 'ntdll.so')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal_unix_ld_args = [
  '-Wl,-install_name,@rpath/winemetal.so',
  winemac_so, ntdll_so,
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-framework', 'MetalFX', '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',  '-weak_framework', 'Cocoa', '-weak_framework', 'IOKit',
  '-lsqlite3',
]
winemetal_unix_link_depends = []
winemetal_unix_c_args = ['-ObjC']
# the bridge's test hooks (MULLION_TEST_GPU_ERROR) exist only in a build with the tests
if get_option('enable_tests')
  winemetal_unix_c_args += '-DMULLION_TEST_HOOKS'
endif

shared_library('winemetal', winemetal_unix_src,
  name_prefix         : '',
  dependencies        : [ airconv_dep_darwin ],
  include_directories : mullion_include_path,
  c_args              : winemetal_unix_c_args,
  link_args           : winemetal_unix_ld_args,
  native              : true,
  name_suffix         : 'so',
  install_rpath       : '',
  build_rpath         : '@loader_path/:@loader_path/../../',
  override_options    : { 'b_asneeded' : false },
)

meson.add_install_script(
  'install.sh', get_option('strip').to_string(), unix_install_dir, 
  install_tag: 'runtime'
)