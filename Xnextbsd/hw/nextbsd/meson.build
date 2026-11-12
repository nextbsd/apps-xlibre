srcs = [
    'nbConfig.c',
    'nbCursor.c',
    'nbFrame.c',
    'nbInit.c',
    'nbInput.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
    '../stubs/ddxBeforeReset.c',
]

# libWindowServerClient and its headers, from NextBSD's WindowServer
# (apps-windowserver).
wsclient_dep = dependency('WindowServerClient')

xnextbsd_server = executable(
    'Xnextbsd',
    srcs,
    include_directories: [inc, '../../miext/rootless'],
    dependencies: [common_dep, wsclient_dep],
    link_with: [
        libxserver_main,
        libxserver_miext_rootless,
        libxserver_fb,
        libxserver,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: true,
)
