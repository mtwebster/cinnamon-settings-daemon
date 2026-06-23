plugin_name = 'background'

background_iface = gnome.gdbus_codegen(
    'csd-exported-background',
    'org.cinnamon.SettingsDaemon.Background.xml',
    interface_prefix: 'org.cinnamon.SettingsDaemon.',
    namespace: 'Csd',
    annotations: ['org.cinnamon.SettingsDaemon.Background', 'org.gtk.GDBus.C.Name', 'ExportedBackground'],
)

background_sources = [
    'bg-geometry.c',
    'bg-renderer.c',
    'bg-source.c',
    'bg-wayland.c',
    'bg-x11.c',
    'csd-background-manager.c',
    'main.c',
    background_iface,
]

background_deps = [
    gtk4,
    gtk4_layer_shell,
    x11_dep,
    cairo_xlib,
    csd_dep,
]

executable(
    'csd-background',
    background_sources,
    include_directories: [include_dirs],
    dependencies: background_deps,
    c_args: [
        '-DG_LOG_DOMAIN="csd-@0@"'.format(plugin_name),
        '-DPLUGIN_NAME="@0@"'.format(plugin_name),
    ],
    install: true,
    install_dir: libexecdir,
)

meson.add_install_script(ln_script, libexecdir, bindir, 'csd-background')
if libexecdir != pkglibdir
    meson.add_install_script(ln_script, libexecdir, pkglibdir, 'csd-background')
endif

configure_file(
    input: 'cinnamon-settings-daemon-background.desktop.in',
    output: 'cinnamon-settings-daemon-background.desktop',
    configuration: desktop_conf,
    install_dir: autostartdir,
)

# Session-bus activation: lets the well-known name be started on demand so a
# readiness watcher can trigger it early and wait on name ownership.
background_service_conf = configuration_data()
background_service_conf.set('LIBEXECDIR', join_paths(prefix, libexecdir))
configure_file(
    input: 'org.cinnamon.SettingsDaemon.Background.service.in',
    output: 'org.cinnamon.SettingsDaemon.Background.service',
    configuration: background_service_conf,
    install_dir: join_paths(datadir, 'dbus-1', 'services'),
)

subdir('tests')
