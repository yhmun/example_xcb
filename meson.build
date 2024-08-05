project('example_xcb', 'cpp',
            license: 'MIT',
            version: '0.10.0',
      meson_version: '>= 0.63.0',
    default_options: [
            'warning_level=3',
            'cpp_std=c++14'
        ]
)

cxx = meson.get_compiler('cpp')
proj_args = cxx.get_supported_arguments(
    '-Wno-unused-parameter',
)
add_project_arguments(proj_args, language: 'cpp')

config_h = configuration_data()
configure_file(output: 'config.h', configuration: config_h)

xcb_dep = dependency('xcb')

apps = [
    {
           'name': 'info',
        'sources': ['info.cpp'],
    },
]

foreach app : apps
    executable('xcb_' + app.get('name'),
                 sources: app.get('sources'),
                cpp_args: [],
     include_directories: [],
               link_with: [],
            dependencies: [xcb_dep],
             install_dir: 'bin' / 'sys',
                  install: true
    )
endforeach
