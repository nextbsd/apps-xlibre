srcs = [
    'nbCapture.c',
    'nbConfig.c',
    'nbCursor.c',
    'nbFrame.c',
    'nbGlamor.c',
    'nbInit.c',
    'nbInput.c',
    'nbRandR.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
    '../stubs/ddxBeforeReset.c',
]

# libWindowServerClient and its headers, from NextBSD's WindowServer
# (apps-windowserver).
wsclient_dep = dependency('WindowServerClient')

# On the GPU (nbGlamor.c): glamor with EGL on GBM, and libdrm for the
# render node; without -Dglamor=true nbGlamor.c builds to stubs and
# Xnextbsd draws with fb only.
xnextbsd_deps = [common_dep, wsclient_dep]
xnextbsd_glamor = []
if build_glamor
    xnextbsd_deps += [epoxy_dep, gbm_dep, libdrm_dep]
    xnextbsd_glamor += glamor
endif

xnextbsd_server = executable(
    'Xnextbsd',
    srcs,
    include_directories: [inc, '../../miext/rootless', glx_inc],
    dependencies: xnextbsd_deps,
    link_with: [
        libxserver_main,
        libxserver_miext_rootless,
        xnextbsd_glamor,
        libxserver_fb,
        libxserver,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: true,
)
