project(
    'post-code-manager',
    'cpp',
    default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=true'],
    license: 'Apache-2.0',
    meson_version: '>=1.1.1',
    version: '1.0',
)

# Disable JSON implicit conversions.
add_project_arguments('-DJSON_USE_IMPLICIT_CONVERSIONS=0', language: 'cpp')

conf_data = configuration_data()
conf_data.set_quoted(
    'DBUS_OBJECT_NAME',
    '/xyz/openbmc_project/State/Boot/PostCode',
)
conf_data.set_quoted('DBUS_INTF_NAME', 'xyz.openbmc_project.State.Boot.PostCode')
conf_data.set_quoted(
    'POSTCODE_DISPLAY_PATH',
    get_option('postcode-display-path'),
)
conf_data.set('MAX_BOOT_CYCLE_COUNT', get_option('max-boot-cycle-count'))
conf_data.set(
    'MAX_POST_CODE_SIZE_PER_CYCLE',
    get_option('max-post-code-size-per-cycle'),
)

if get_option('bios-post-code-log').allowed()
    add_project_arguments('-DENABLE_BIOS_POST_CODE_LOG', language: 'cpp')
endif

configure_file(output: 'config.h', configuration: conf_data)

sdbusplus = dependency('sdbusplus')
phosphor_logging = dependency('phosphor-logging')
phosphor_dbus_interfaces = dependency('phosphor-dbus-interfaces')
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

systemd_system_unit_dir = dependency('systemd').get_variable(
    'systemd_system_unit_dir',
)

install_subdir(
    'service_files',
    install_dir: systemd_system_unit_dir,
    strip_directory: true,
)

packagedir = join_paths(
    get_option('prefix'),
    get_option('datadir'),
    'phosphor-post-code-manager',
)

handlers_variant = get_option('post-code-handlers')
if handlers_variant != ''
    handler_config = files(
        'config/post-code-handlers-' + handlers_variant + '.json',
    )
else
    handler_config = files('config/post-code-handlers.json')
endif

config_validator = files('scripts/schema_validator.py')
confchecker_iface = custom_target(
    'check_syntax_iface',
    command: [
        'python3',
        config_validator,
        '--schema',
        files('schema/handlers.json'),
        '@INPUT@',
    ],
    input: handler_config,
    depend_files: handler_config,
    build_by_default: true,
    capture: true,
    output: 'validate_configs.log',
)
install_data(
    sources: handler_config,
    install_dir: packagedir,
    rename: 'post-code-handlers.json',
)

executable(
    'post-code-manager',
    'src/main.cpp',
    'src/post_code.cpp',
    'src/nvidia_post_code_handler.cpp',
    install: true,
    dependencies: [
        sdbusplus,
        phosphor_dbus_interfaces,
        phosphor_logging,
        cereal_dep,
        json,
    ],
    include_directories: 'inc',
)

build_tests = get_option('tests')

if build_tests.allowed()
    subdir('test')
endif

if get_option('b_coverage')
    gcovr = find_program('gcovr', required: false)
    if gcovr.found()
        gcovr_config_path = join_paths(meson.project_source_root(), 'gcovr.cfg')
        coverage_html_dir = meson.current_build_dir() / 'coverage-html'
        coverage_html_out = coverage_html_dir / 'index.html'
        mkdir = find_program('mkdir')
        sh = find_program('sh')
        coverage_html = custom_target(
            'coverage-html-gcovr',
            output: 'coverage-html-gcovr',
            command: [
                sh,
                '-c',
                '@0@ -p @1@ && @2@ --config @3@ --root @4@ --html --html-details -o @5@ --print-summary'.format(
                    mkdir.path(),
                    coverage_html_dir,
                    gcovr.path(),
                    gcovr_config_path,
                    meson.project_source_root(),
                    coverage_html_out,
                ),
            ],
            depends: [],
            build_by_default: false,
        )
    endif
endif
