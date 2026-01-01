dep_wayland_protocols = dependency('wayland-protocols', version: '>= 1.45')
dep_wayland_scanner = dependency('wayland-scanner', native: true)

program_scanner = find_program(dep_wayland_scanner.get_variable('wayland_scanner'))
dep_wayland_protocols_base = dep_wayland_protocols.get_variable('pkgdatadir')

protocol_dir = include_directories('.')

generated_protocols = [
    [ 'xdg-shell', 'stable' ],
]

foreach proto: generated_protocols
	proto_name = proto[0]
	if proto[1] == 'internal'
		base_file = proto_name
		xml_path = '@0@.xml'.format(proto_name)
	elif proto[1] == 'stable'
		base_file = proto_name
		xml_path = '@0@/stable/@1@/@1@.xml'.format(dep_wayland_protocols_base, base_file)
	else
		base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
		xml_path = '@0@/unstable/@1@/@2@.xml'.format(dep_wayland_protocols_base, proto_name, base_file)
	endif

	foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
		if output_type == 'client-header'
			output_file = '@0@-client-protocol.h'.format(base_file)
		elif output_type == 'server-header'
			output_file = '@0@-protocol.h'.format(base_file)
		else
			output_file = '@0@-protocol.c'.format(base_file)
			if dep_wayland_scanner.version().version_compare('< 1.14.91')
				output_type = 'code'
			endif
		endif

		var_name = output_file.underscorify()
		target = custom_target(
			'@0@ @1@'.format(base_file, output_type),
			command: [ program_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
			input: xml_path,
			output: output_file,
		)

		set_variable(var_name, target)
	endforeach
endforeach