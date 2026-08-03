import os
import platform
import subprocess
import hashlib
import shutil
from SCons.Script import Exit

def generate(env):
    env.AddMethod(_install_vcpkg, "InstallVcpkg")
    env.AddMethod(_install_deps, "InstallDependencies")

def exists(env):
    return True

def _get_vcpkg_triplet():
    """
    Detect the vcpkg triplet for the current host system.
    Queries the actual installed directories to determine what was built.
    """
    system = platform.system()
    machine = platform.machine().lower()

    # Map various machine names to vcpkg architecture names
    arch_map = {
        'aarch64': 'arm64',
        'arm64': 'arm64',
        'x86_64': 'x64',
        'amd64': 'x64',
        'x64': 'x64',
        'armv7l': 'arm',
        'armv7': 'arm',
    }

    arch = arch_map.get(machine, machine)
    
    if system == "Windows":
        return "x64-windows"
    elif system == "Darwin":  # macOS
        return f"{arch}-osx"
    elif system == "Linux":
        return f"{arch}-linux"
    return None

def _install_vcpkg(env):
    vcpkg_path = f"./thirdparty/vcpkg"
    bootstrap_script = f"{vcpkg_path}/bootstrap-vcpkg.sh"

    if not os.path.exists(bootstrap_script):  # Check for a known file
        # Remove empty or incomplete directory before cloning
        if os.path.exists(vcpkg_path):
            print("Cleaning up inclomplete VCPKG folder")
            shutil.rmtree(vcpkg_path)

        print("Cloning VCPKG")
        result = subprocess.run([
            "git", "clone",
            "https://github.com/microsoft/vcpkg.git",
            vcpkg_path
        ])

        if result.returncode != 0:
            print(f"Failed to clone VCPKG repo.")
            Exit(f"Build aborted due to subprocess failure (exit code: {result.returncode})")

    if not os.path.exists(f"{vcpkg_path}/packages"):
        vcpkg_path_abs = os.path.abspath(vcpkg_path)
        if platform.system() == "Windows":
            script_type = "bat"
        else:
            script_type = "sh"

        result = subprocess.run([
            f"{vcpkg_path_abs}/bootstrap-vcpkg.{script_type}"
        ])
        if result.returncode != 0:
            print(f"Failed to bootstrap VCPKG repo.")
            Exit(f"Build aborted due to subprocess failure (exit code: {result.returncode})")

    vcpkg_tripplet = _get_vcpkg_triplet()
    env['VCPKG_TRIPLET'] = vcpkg_tripplet

def _compute_vcpkg_hash(vcpkg_json_path):
    """Compute SHA256 hash of vcpkg.json file."""
    if not os.path.exists(vcpkg_json_path):
        return None
    
    with open(vcpkg_json_path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()

def _read_cached_hash(marker_file):
    """Read the cached hash from the marker file."""
    if not os.path.exists(marker_file):
        return None
    
    try:
        with open(marker_file, 'r') as f:
            return f.read().strip()
    except:
        return None

def _write_cached_hash(marker_file, hash_value):
    """Write the hash to the marker file."""
    try:
        with open(marker_file, 'w') as f:
            f.write(hash_value)
    except Exception as e:
        print(f"Warning: Could not write cache marker file: {e}")

def _install_deps(env):
    vcpkg_path = os.path.abspath(f"./thirdparty/vcpkg")
    vcpkg_json_path = os.path.join(".", "vcpkg.json")
    marker_file = os.path.join("thirdparty", ".vcpkg_installed_hash")

    # Compute current hash of vcpkg.json
    current_hash = _compute_vcpkg_hash(vcpkg_json_path)

    # Read cached hash
    cached_hash = _read_cached_hash(marker_file)

    # Check if vcpkg_installed directory exists
    vcpkg_installed_exists = os.path.exists(os.path.join("thirdparty", "vcpkg_installed"))

    # Determine if we need to install
    needs_install = (
        current_hash is None or  # No vcpkg.json found
        cached_hash is None or   # No cache file
        current_hash != cached_hash or  # Dependencies changed
        not vcpkg_installed_exists  # Installation directory missing
    )

    if needs_install:
        print("Installing vcpkg dependencies (dependencies changed or not yet installed)...")
        vcpkg_env = os.environ.copy()
        vcpkg_env["VCPKG_INSTALLED_DIR"] = "./thirdparty/vcpkg_installed"
        vcpkg_env["VCPKG_DISABLE_METRICS"] = "True"
        result = subprocess.run([
            f"{vcpkg_path}/vcpkg",
            "--vcpkg-root", vcpkg_path,
            "--x-install-root", "./thirdparty/vcpkg_installed",
            "install"
        ],
        env=vcpkg_env)

        if result.returncode != 0:
            # Fail hard: continuing past a failed vcpkg install leads to
            # confusing follow-up errors (missing headers, missing protoc,
            # link failures) that look like "random" build breakage.
            print(f"ERROR: vcpkg install failed with exit code {result.returncode}.")

            # Best-effort: remove the hash marker so the next build re-attempts install
            try:
                if os.path.exists(marker_file):
                    os.remove(marker_file)
            except Exception:
                pass
            Exit(f"Build aborted: vcpkg install failed (exit code: {result.returncode})")

        if current_hash:
            # Update the cache marker on successful installation
            _write_cached_hash(marker_file, current_hash)
        print("vcpkg dependencies installed successfully.")
    else:
        print("vcpkg dependencies are up-to-date (skipping installation).")
