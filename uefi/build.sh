#!/bin/bash
#
# Fetch the EDK2 / Mu dependencies and build the Kirin 9010 (charlotte) UEFI.
#
# Usage:
#   ./build.sh              # Debug build
#   ./build.sh RELEASE      # Release build
#
set -e

cd "$(dirname "$0")"

TARGET="${1:-DEBUG}"

install_dependencies() {
  if ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    echo "aarch64-linux-gnu-gcc is missing"
    exit 1
  fi

  if ! command -v stuart >/dev/null; then
    python3 -m pip install -r pip-requirements.txt
  fi
}

clone_dependency() {
  local url="$1"
  local path="$2"
  local branch="$3"

  if [ -d "${path}/.git" ]; then
    return
  fi

  if [ -n "${branch}" ]; then
    git clone --depth 1 -b "${branch}" "${url}" "${path}"
  else
    git clone --depth 1 "${url}" "${path}"
  fi
}

install_dependencies

clone_dependency https://github.com/microsoft/mu_basecore.git Mu_Basecore release/202608
clone_dependency https://github.com/microsoft/mu_plus.git Common/Mu release/202608
clone_dependency https://github.com/microsoft/mu_oem_sample.git Common/Mu_OEM_Sample release/202608
clone_dependency https://github.com/Project-Silicium/Silicium-ACPI.git Silicium-ACPI ""
clone_dependency https://github.com/openssl/openssl.git \
  Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl \
  67b5686b4419b4cb8caa502711c41815f5279751

python3 build_uefi.py -d mro -r "${TARGET}" -c
