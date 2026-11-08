# ==============================================================================
# rt2800usb module — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/x86_64-cact-clang.ini \
#       -Dkern_root=../CactKernel-x86_64 -Dlocal_repo=../LocalRepoCactOS-x86_64
#   ninja -C build-meson          # build-meson/rt2800usb.cctk
#   ninja -C build-meson stage    # copy into <local_repo>/lib
#
# The .cctk is a single relocatable object (ET_REL): every source file is
# compiled to an i386 object and the objects are combined with a relocatable
# link (`clang -r`), because the kernel's USB module loader maps one ET_REL.
# ==============================================================================

project('rt2800usb-mod', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=1',
    'buildtype=plain',
  ])

fs = import('fs')
bash = find_program('bash')
cc = meson.get_compiler('c')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_kern = get_option('kern_root')
kern_root = _kern.startswith('/') ? _kern : src_root / _kern
_local = get_option('local_repo')
local_repo = _local.startswith('/') ? _local : src_root / _local
install_dir = local_repo / 'lib'

# Kernel headers this module compiles against.
mod_includes = [
  kern_root / 'Cact/drivers/usb',
  kern_root / 'Cact/net',
  kern_root / 'Cact/net/wireless',
  kern_root / 'Cact/kernel/sync',
  kern_root / 'Cact/kernel/core/syscall',
  kern_root / 'Cact/fs/vfs',
  kern_root / 'Cact/fs/vfs/devfs',
]

mod_c_args = [
  '-ffreestanding',
  '-fno-pie',
  '-fno-stack-protector',
  '-nostdlib',
  # Ring 0: the kernel has not enabled SSE for itself, so clang must not lower
  # struct copies to SSE2 (the same trap the kernel build hits).
  '-mno-mmx',
  '-mno-sse',
  '-mno-sse2',
  '-O2',
]
foreach d : mod_includes
  mod_c_args += '-I' + d
endforeach
mod_c_args += '-I' + src_root

_cc_cmd  = ' '.join(cc.cmd_array())
_args    = ' '.join(mod_c_args)

_sources = ['rt2800usb.c', 'rt2800_init.c', 'rt2800_mac.c', 'rt2800_bbp_rf.c',
            'rt2800_usb_rings.c', 'rt2800_channel.c', 'rt2800_cact80211.c']

rt2800usb_cctk = custom_target('rt2800usb.cctk',
  input: _sources,
  output: 'rt2800usb.cctk',
  # Headers are not compiled, so list them as depend_files: without this a
  # header-only edit (e.g. RT_REG_TRACE) would silently not rebuild the module.
  depend_files: ['rt2800_local.h', 'rt2x00_compat.h', 'rt2800_regs.h',
                 kern_root / 'Cact/net/wireless/cact80211.h'],
  command: [bash, '-c', (
    'set -e; ' +
    'out="$1"; shift; ' +
    'd=$(mktemp -d); ' +
    'for f in "$@"; do ' +
      _cc_cmd + ' -m64 ' + _args + ' -c "$f" -o "$d/$(basename "$f" .c).o"; ' +
    'done; ' +
    _cc_cmd + ' -m64 -nostdlib -r "$d"/*.o -o "$out"; ' +
    'rm -rf "$d"'),
    'bash', '@OUTPUT@', '@INPUT@'],
  build_by_default: true)

# Meson reserves the name "install" for itself, hence "stage".
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/rt2800usb.cctk"',
            'bash', install_dir, rt2800usb_cctk])
