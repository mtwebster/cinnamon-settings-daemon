test_geometry = executable(
    'test-geometry',
    ['test-geometry.c', '../bg-geometry.c'],
    include_directories: [include_dirs, include_directories('..')],
    dependencies: [glib],
)
test('geometry', test_geometry)

test_renderer = executable(
    'test-renderer',
    ['test-renderer.c', '../bg-renderer.c', '../bg-geometry.c'],
    include_directories: [include_dirs, include_directories('..')],
    dependencies: [gtk4],
)
test('renderer', test_renderer)
