project('rtspunch', 'c',
  version: '0.1',
  default_options: [
    'warning_level=0',
    'prefix=' + meson.source_root()
  ]
)

src = files(
    'src/http.c',
    'src/rtsp.c',
    'src/rtp.c',
    'src/rtcp.c',
    'src/stun.c',
    'src/logs.c',    
    'src/config.c',
)

ldflags = ['-lm', '-lz', '-pthread']
if host_machine.system() != 'darwin'
  ldflags += ['-static']
endif

exe = executable('rtspunch', src,
    install: true,
    install_dir: 'bin',
    cpp_args: ['-g', '-O0', '-Wall'],
    link_args: ldflags
)