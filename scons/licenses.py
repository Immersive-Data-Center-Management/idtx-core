"""
SCons tool: licenses

Usage in SConstruct:
    env.CopyLicenseFiles()
"""
import configparser
import datetime
import glob
import os
import platform
import re
import shutil
import stat
from SCons.Script import Exit

def generate(env):
    env.AddMethod(_copy_third_party_licenses, 'CopyLicenseFiles')

def exists(env):
    return True


def _copy_third_party_licenses(env):
    """Copy third-party LICENSE files for distribution compliance."""
    print("Copying third-party LICENSE files...")

    license_dest_dir = "bin/LICENSES-THIRD-PARTY"
    os.makedirs(license_dest_dir, exist_ok=True)

    open_usd_src_path = env["OPENUSD_SRC_PATH"]
    license_files = [
        (f"{open_usd_src_path}/LICENSE.txt", "openusd-LICENSE.txt"),
        (f"{open_usd_src_path}/NOTICE.txt", "openusd-NOTICE.txt"),
        (f"tests/thirdparty/doctest/LICENSE.txt", "doctest-LICENSE.txt"),
    ]

    missing = []
    for src, dest_name in license_files:
        if os.path.exists(src):
            dest_path = os.path.join(license_dest_dir, dest_name)
            shutil.copy2(src, dest_path)
            # copy2 preserves source permissions; some SDKs ship read-only files,
            # which would cause a Permission denied error on the next incremental build.
            os.chmod(dest_path, os.stat(dest_path).st_mode | stat.S_IRUSR | stat.S_IWUSR)
            print(f"  Copied: {src} -> {dest_path}")
        else:
            missing.append(src)

    if os.path.exists("THIRDPARTY.txt"):
        cfg = configparser.ConfigParser()
        cfg.read("bin/version.cfg")
        version = cfg.get("application", "version", fallback="unknown").strip('"').strip("'")
        if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[\w.]+)?(?:\+[\w.]+)?", version):
            print(f"ERROR: Version '{version}' in version.cfg does not follow semver (MAJOR.MINOR.PATCH).")
            return 1
        today = datetime.date.today()
        date_str = f"{today.strftime('%B')} {today.day}, {today.year}"
        with open("THIRDPARTY.txt", "r") as f:
            lines = f.read().splitlines(keepends=True)
        lines[0] = f"IDTX Core - Version {version} - {date_str}\n"
        with open("bin/THIRDPARTY.txt", "w") as f:
            f.writelines(lines)
        print(f"  Stamped THIRDPARTY.txt with version {version} and date {date_str}")

    if missing:
        print("ERROR: The following LICENSE files are missing and must be present for distribution compliance:")
        for f in missing:
            print(f"  {f}")
        return 1

