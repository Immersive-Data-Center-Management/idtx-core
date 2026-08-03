# python b uild script for scons building
import os
import platform
import glob
import shutil

from SCons.Script import ARGUMENTS

# USD Version and path configuration
openusd_version = "26.08"
shared_thirdparty_root = "./thirdparty"

# allow developer to overwrite those paths
try:
    import custom
except ImportError:
    custom = None

if custom:
    openusd_version = getattr(custom, "OPENUSD_VERSION", openusd_version)
    shared_thirdparty_root = getattr(custom, "SHARED_THIRDPARTY_ROOT", shared_thirdparty_root)

usd_root = f"{shared_thirdparty_root}/openusd-{openusd_version}"
usd_src = f"{shared_thirdparty_root}/openusd-{openusd_version}-src"

def find_absl_libs(lib_dir, extension):
    libs = []
    for path in glob.glob(os.path.join(lib_dir, f"*absl_*.{extension}")):
        libname = os.path.basename(path)
        if libname.endswith(f".{extension}"):
            if libname.startswith("lib"):
                lib = libname[3:-2]  # strip "lib" prefix and ".a" suffix
            else:
                lib = libname
            libs.append(lib)
    return libs

def _get_libs_to_install(platform_name):
    if platform_name == "windows":
        usd_libs = [
            f"{usd_root}/lib/usd_ms.dll",
            f"{usd_root}/bin/tbb12.dll"
        ]
    elif platform_name == "macos":
        usd_libs = [
            f"{usd_root}/lib/libusd_ms.dylib",
            f"{usd_root}/lib/libtbb.12.dylib"
        ]
    else:
        usd_libs = [
            f"{usd_root}/lib/libusd_ms.so"            
        ]
    
    if platform_name == "windows":
        libs_to_install = usd_libs + [
            f"./thirdparty/vcpkg_installed/x64-windows/bin/abseil_dll.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/libprotobuf.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/boost_url-vc143-mt-x64-1_91.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/cpr.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/libcurl.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/z.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/libssl-3-x64.dll",
            f"./thirdparty/vcpkg_installed/x64-windows/bin/libcrypto-3-x64.dll",
        ]
    else:
        libs_to_install = usd_libs
        
    return libs_to_install

def _copy_usd_plugins(target, source, env):
    print("Copy USD Plugin Config..")
    shutil.copytree(f"{usd_root}/lib/usd", f"bin/usd", dirs_exist_ok=True)
    shutil.copytree(f"{usd_root}/plugin/usd", f"bin/plugin/usd", dirs_exist_ok=True)

# configure the main environment to use the different tools to build all we need
env = Environment(
    ENV=os.environ.copy(),
    tools=["default", "openusd", "vcpkg", "protobuf", "licenses"],
    toolpath=["scons"],
    MSVC_VERSION='14.3',
    OPENUSD_VERSION=openusd_version,
    OPENUSD_PATH=usd_root,
    OPENUSD_SRC_PATH=usd_src
)

env.InstallVcpkg()
env.InstallDependencies()
env.CompileProtobuffer()

if platform.system() == "Windows":
    env["PLATFORM"] = "windows"
elif platform.system() == "Darwin":
    env["PLATFORM"] = "macos"
else:
    env["PLATFORM"] = "linux"

env['platform_name'] = env["PLATFORM"]
env['arch'] = ARGUMENTS.get('arch', 'arm64')
env['target'] = ARGUMENTS.get('target', 'debug')

platform_name = env["platform_name"]
build_target = env["target"]
build_arch = env["arch"]
vcpkg_triplet = env['VCPKG_TRIPLET']
print(f"Using vcpkg triplet: {vcpkg_triplet}")

# generic build flags
if platform.system() == "Windows" and (env["CXX"] == "cl" or env["CC"] == "cl"):
    # MSVC: Enable C++20
    env.Append(CXXFLAGS=['/std:c++20', '/EHsc', '/GR', '/FS'])
    env.Append(CCFLAGS=["-O3" if build_target == "release" else "/Zi"])
else:
    # GCC/Clang: Enable C++20
    env.Append(CXXFLAGS=['-std=c++20', '-fexceptions', '-frtti', '-g'])
    env.Append(CCFLAGS=["-O3" if build_target == "release" else "-g"])
    if platform.system() == "Darwin":  # Only add -arch on macOS
        env.Append(CCFLAGS=['-arch', env['arch']])

env.BuildOpenUSD()

env.Append(CPPPATH=[
        f"{usd_root}/include",
        f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/include",
        "./shared/include",
        "./source"
	])

# Libraries to link
if platform_name == "windows":
    absl_libs = find_absl_libs(f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/lib", "lib")
    absl_libs.append("abseil_dll")
elif platform_name == "macos":
    absl_libs = find_absl_libs(f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/lib", "a")
else:
    absl_libs = find_absl_libs(f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/lib", "a")
    
libs = [
    "usd_ms",
    "libprotobuf" if platform_name == "windows" else "protobuf",
    #"boost_system-vc143-mt-x64-1_91" if platform_name == "windows" else "boost_system",
    "boost_url-vc143-mt-x64-1_91" if platform_name == "windows" else "boost_url",
    "utf8_range",
    "utf8_validity",
    "cpr",
    "libcurl",
    "libssl",
    "libcrypto"
] + absl_libs

if platform_name == "windows":
    libs.extend(["tbb12"])
elif platform_name == "macos":
    libs.extend(["tbb.12"])

# Platform-specific libraries
if platform.system() == 'Darwin':
    libs.extend(['c++', 'System', 'crypto', 'ssl', 'z'])
elif platform.system() == 'Windows':
    libs.extend(['ws2_32', 'wsock32'])
elif platform.system() == 'Linux':
    libs.extend(['crypto', 'ssl', 'z'])

env.Append(LIBPATH=[
    f"{usd_root}/lib",
    f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/lib"
    ])

# Platform-specific configuration
if platform_name == "linux":
    env.Append(LIBS=libs + ["dl", "pthread", "m"])
    env.Append(CCFLAGS=["-fPIC", "-g"])
    env.Append(LINKFLAGS=["-Wl,-rpath,$ORIGIN"])
    # Wrap all libraries in linker group to handle circular dependencies in static libraries
    env['_LIBFLAGS'] = '-Wl,--start-group ' + env['_LIBFLAGS'] + ' -Wl,--end-group'

    # Try to find OpenSSL
    openssl_paths = [
        '/usr/lib/x86_64-linux-gnu',  # Ubuntu/Debian x64
        '/usr/lib/aarch64-linux-gnu',  # Ubuntu/Debian ARM64
        '/usr/lib64',                  # RHEL/CentOS x64
        '/usr/lib',                    # Fallback
    ]
    
    for openssl_lib_path in openssl_paths:
        if os.path.exists(openssl_lib_path):
            env.Append(LIBPATH=[openssl_lib_path])
            break
    
    # Add standard OpenSSL include path
    env.Append(CPPPATH=['/usr/include'])

elif platform_name == "windows":
    env.Append(LIBS=libs + ["advapi32", "shell32", "ole32"])
    env.Append(CPPDEFINES=["NOMINMAX", "WIN32_LEAN_AND_MEAN", "_WIN32_WINDOWS"])
    env.Append(LINKFLAGS=['/SUBSYSTEM:CONSOLE'])
    # deactivate this warning. This appears due to an issue in openUSD-26.05 where the definition of
    # 'std::ostream &Vt_ArrayEditStreamImpl()' is missing the 'VT_API' decorator
    env.Append(CCFLAGS=['/wd4273'])
    
    # Force use of release TBB library even in debug builds since debug version isn't available
    if build_target != "release":
        env.Append(LINKFLAGS=['/NODEFAULTLIB:tbb12_debug.lib', '/DEFAULTLIB:tbb12.lib'])
        # DEBUG
        env.Append(CCFLAGS=[
            "/Zi",        # debug symbols
            "/Od",        # no optimization
            "/EHsc",
            "/MT"
        ])
        env.Append(LINKFLAGS=[
            "/DEBUG"      # generate PDB (REQUIRED)
        ])
    else:
        # RELEASE
        env.Append(CCFLAGS=[
            "/O2",
            "/EHsc",
            "/MT"
        ])

elif platform_name == "macos":
    env.Append(LIBS=libs + ["pthread", "crypto", "ssl"])
    env.Append(CCFLAGS=["-fPIC", "-g"])
    env.Append(LINKFLAGS=["-framework", "CoreFoundation"])
    env.Append(LINKFLAGS=["-framework", "SystemConfiguration"])
    env.Append(LINKFLAGS=["-Wl,-rpath,@loader_path"])
    brew_prefix = '/opt/homebrew' if platform.machine() == 'arm64' else '/usr/local'
    env.Append(CPPPATH=[os.path.join(brew_prefix, 'include')])
    env.Append(LIBPATH=[os.path.join(brew_prefix, 'lib')])
    openssl3 = os.path.join(brew_prefix, 'opt', 'openssl@3')
    openssl11 = os.path.join(brew_prefix, 'opt', 'openssl@1.1')
    if os.path.exists(openssl3):
        env.Append(LIBPATH=[os.path.join(openssl3, 'lib')])
        env.Append(CPPPATH=[os.path.join(openssl3, 'include')])
    elif os.path.exists(openssl11):
        env.Append(LIBPATH=[os.path.join(openssl11, 'lib')])
        env.Append(CPPPATH=[os.path.join(openssl11, 'include')])

env.Append(CPPDEFINES=["CROW_ENABLE_WEBSOCKET"])

sources = list(set(env.Glob("source/*.cpp") + env.Glob("source/**/*.cpp") + env.Glob(f"./shared/build/proto/*.pb.cc")))

# Build the executable
program = env.Program(
    target='build/idtx-core',
    source=sources,
    LIBS=libs
)

# Add install target
install_dir = f"bin"
install_ext = env.Install(install_dir, program)
install_libs = env.Install(install_dir, _get_libs_to_install(platform_name))
env.AddPostAction(program, _copy_usd_plugins)

env.Default(program, install_ext + install_libs)

env.CopyLicenseFiles()

# ---------------------------------------------------------------------------
# Opt-in test build: `scons tests=1` compiles tests/*.cpp into
# bin/idtx-core-tests(.exe) and links against the same object files the
# server binary uses (source/**/*.cpp minus source/main.cpp).
# ---------------------------------------------------------------------------
if ARGUMENTS.get('tests', '0') != '0':
    SConscript(
        'tests/SConscript',
        exports={
            'env': env,
            'libs': libs,
            'usd_root': usd_root,
            'shared_thirdparty_root': shared_thirdparty_root,
        }
    )
