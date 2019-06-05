wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')
wayland_dir = wayland_client.get_pkgconfig_variable('pkgdatadir')

wayland_scanner_prog = find_program(wayland_scanner.get_pkgconfig_variable('wayland_scanner'))

wayland_scanner_data = generator(
	wayland_scanner_prog,
	output: '@BASENAME@-data.c',
	arguments: ['public-code', '@INPUT@', '@OUTPUT@'],
)

# TODO: modify wayland scanner to produce only the required information to trace the protocol
wayland_scanner_client = generator(
	wayland_scanner_prog,
	output: '@BASENAME@-client-defs.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)
wayland_scanner_server = generator(
	wayland_scanner_prog,
	output: '@BASENAME@-server-defs.h',
	arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

protocols = [
    [wayland_dir, 'wayland.xml'],
    [wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
    [wl_protocol_dir, 'stable/presentation-time/presentation-time.xml'],
    [wl_protocol_dir, 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'],
    ['gtk-primary-selection.xml'],
    ['input-method-unstable-v2.xml'],
    ['virtual-keyboard-unstable-v1.xml'],
    ['wlr-screencopy-unstable-v1.xml'],
    ['wayland-drm.xml'],
]

protocols_src = []
protocols_headers = []
foreach path_parts : protocols
	xml = join_paths(path_parts)
	protocols_src += wayland_scanner_data.process(xml)
	protocols_headers += wayland_scanner_client.process(xml)
	protocols_headers += wayland_scanner_server.process(xml)
endforeach

# the src files do not require the headers
lib_protocols = static_library(
	'protocols',
	protocols_src,
	dependencies: [wayland_client] # for the header files, only
)
protos = declare_dependency(
	link_with: lib_protocols,
	sources: protocols_headers,
)