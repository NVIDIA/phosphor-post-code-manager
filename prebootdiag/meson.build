# Started by the Vera handlers through
# com.nvidia.PreBootDiag.PostCodeCallback@<name>.service.
install_prebootdiag = handlers_variant == 'vera'

prebootdiag_lib = static_library(
    'prebootdiag_notify',
    'postcode_notify.cpp',
    build_by_default: install_prebootdiag,
)

executable(
    'prebootdiag-postcode-notify',
    'main.cpp',
    link_with: prebootdiag_lib,
    dependencies: [sdbusplus],
    build_by_default: install_prebootdiag,
    install: install_prebootdiag,
)

if install_prebootdiag
    install_data(
        'com.nvidia.PreBootDiag.PostCodeCallback@.service',
        install_dir: systemd_system_unit_dir,
    )
endif

if build_tests.allowed()
    test(
        'test_postcode_notify',
        executable(
            'test_postcode_notify',
            'test_postcode_notify.cpp',
            link_with: prebootdiag_lib,
            dependencies: [
                dependency('gtest', main: true, required: build_tests),
            ],
        ),
        protocol: 'gtest',
    )
endif
