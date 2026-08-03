import os.path
import platform
import shutil
import glob
import subprocess
from SCons.Script import Exit

def generate(env):
    env.AddMethod(_compile_protobuf, "CompileProtobuffer")

def exists(env):
    return True

def _compile_protobuf(env):
    print("Compiling Protobuf Messages...")
    
    shared_deps_root = env.get('SHARED_DEPS_ROOT', '.')
    # Use shared build directory and proto messages
    shared_build_dir = f"shared/build/proto"
    shared_proto_dir = f"shared/proto_messages"
    shared_include_dir = f"shared/include/idtx/proto"
    
    # Create shared build directory
    mkdir_flag = "-p" if platform.system() != "Windows" else ""
    env.Execute(f"mkdir {mkdir_flag} {os.path.normpath(shared_build_dir)}")
    
    # Derive a sensible fallback triplet from the current host if VCPKG_TRIPLET
    # was not populated by the vcpkg tool (which happens if InstallVcpkg was
    # skipped). Never hard-code an arch here: on x64 Linux hosts a hard-coded
    # arm64-linux fallback would silently point at a non-existent
    # vcpkg_installed/ subdirectory and cause spurious "missing package" errors.
    machine = platform.machine().lower()
    arch_map = {
        'aarch64': 'arm64', 'arm64': 'arm64',
        'x86_64': 'x64', 'amd64': 'x64', 'x64': 'x64',
        'armv7l': 'arm', 'armv7': 'arm',
    }
    arch = arch_map.get(machine, machine)

    if platform.system() == "Windows":
        platform_path = "x64-windows"
    elif platform.system() == "Darwin":
        platform_path = f"{arch}-osx"
    else:
        platform_path = f"{arch}-linux"

    vcpkg_triplet = env.get('VCPKG_TRIPLET', platform_path)
    print(f"Using vcpkg triplet: {vcpkg_triplet}")
    compiler = os.path.normpath(f"./thirdparty/vcpkg_installed/{vcpkg_triplet}/tools/protobuf/protoc")
    
    result = subprocess.run([
        compiler,
        f"--cpp_out={os.path.normpath(shared_build_dir)}",
        f"--proto_path={os.path.normpath(shared_proto_dir)}",
        f"{os.path.normpath('base.proto')}",
        f"{os.path.normpath('transform.proto')}",
        f"{os.path.normpath('session.proto')}"
    ])
    if result.returncode != 0:
        print(f"Failed to compile protobuffer")
        Exit(f"Build aborted due to subprocess failure (exit code: {result.returncode})")

    # Copy generated header files to the shared include folder so they can be
    # consumed by other components via a stable include path: idtx/proto/*.pb.h
    print(f"Copying generated protobuf headers to {shared_include_dir}...")
    os.makedirs(os.path.normpath(shared_include_dir), exist_ok=True)

    header_patterns = ["*.pb.h", "*.pb.hpp"]
    copied_files = []
    for pattern in header_patterns:
        for header in glob.glob(os.path.join(shared_build_dir, pattern)):
            destination = os.path.join(shared_include_dir, os.path.basename(header))
            shutil.copy2(header, os.path.normpath(destination))
            copied_files.append(destination)

    if not copied_files:
        print("Warning: No generated protobuf header files were found to copy.")
    else:
        for f in copied_files:
            print(f"  Copied: {os.path.normpath(f)}")