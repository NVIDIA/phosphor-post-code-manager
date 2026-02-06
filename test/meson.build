# GTest/GMock from system or env (no wrap)
gtest = dependency(
    'gtest_main',
    main: true,
    version: '>=1.15.0',
    required: build_tests,
)
gmock = dependency('gmock', required: build_tests)

# Dependencies
phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
sdbusplus = dependency('sdbusplus')
phosphor_logging = dependency('phosphor-logging')
json = dependency('nlohmann_json', include_type: 'system')

cxx = meson.get_compiler('cpp')
cereal_dep = dependency('cereal', required: false)
has_cereal = cxx.has_header_symbol(
    'cereal/cereal.hpp',
    'cereal::specialize',
    dependencies: cereal_dep,
    required: false,
)
if not has_cereal
    cereal_opts = import('cmake').subproject_options()
    cereal_opts.add_cmake_defines(
        {'BUILD_TESTS': 'OFF', 'SKIP_PERFORMANCE_COMPARISON': 'ON'},
    )
    cereal_proj = import('cmake').subproject(
        'cereal',
        options: cereal_opts,
        required: false,
    )
    assert(cereal_proj.found(), 'cereal is required')
    cereal_dep = cereal_proj.dependency('cereal')
endif

# Include directories
test_include_dirs = include_directories('../inc', '..')

# Test-specific configuration
test_cpp_args = ['-DJSON_USE_IMPLICIT_CONVERSIONS=0']

# Enable BIOS POST code log for test coverage
if get_option('bios-post-code-log').allowed()
    test_cpp_args += ['-DENABLE_BIOS_POST_CODE_LOG']
endif

# Source files to link with tests
test_sources = ['../src/post_code.cpp', '../src/nvidia_post_code_handler.cpp']

# Test dependencies
test_deps = [
    gtest,
    gmock,
    sdbusplus,
    phosphor_dbus_interfaces,
    phosphor_logging,
    cereal_dep,
    json,
]

# Test executables
tests = [
    'test_post_code',
    'test_nvidia_post_code_handler',
    'test_post_code_handlers',
]

foreach t : tests
    test(
        t,
        executable(
            t.underscorify(),
            t + '.cpp',
            test_sources,
            include_directories: test_include_dirs,
            implicit_include_directories: false,
            dependencies: test_deps,
            cpp_args: test_cpp_args,
        ),
        protocol: 'gtest',
    )
endforeach
