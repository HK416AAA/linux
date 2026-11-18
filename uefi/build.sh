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

# Commits used by the Project Silicium reference tree (uefi-sheng)
MU_BASECORE_SHA="cfb93724a06ff86ff5ba971413368a7809ee42a5"
MU_SHA="b8d46c71610318e36a661819e1e3c0b8bdda413a"
MU_OEM_SHA="b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6"
SILICIUM_ACPI_SHA="2d465ad77c25de908f0ec7bdfcb57f865b61233c"
OPENSSL_SHA="67b5686b4419b4cb8caa502711c41815f5279751"

install_dependencies() {
  if ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    echo "aarch64-linux-gnu-gcc is missing"
    exit 1
  fi

  if ! command -v stuart >/dev/null; then
    python3 -m pip install -r pip-requirements.txt
  fi
}

# The reference tree pins every dependency by commit hash, so fetch the
# commit directly instead of tracking a moving branch tip.
fetch_pinned() {
  local path="$1"
  local url="$2"
  local sha="$3"

  if [ -d "${path}/.git" ]; then
    return
  fi

  mkdir -p "${path}"
  git -C "${path}" init -q
  git -C "${path}" remote add origin "${url}"

  if ! git -C "${path}" fetch -q --depth 1 origin "${sha}"; then
    git -C "${path}" fetch -q origin
  fi

  git -C "${path}" checkout -q "${sha}"
}

install_dependencies

fetch_pinned Mu_Basecore https://github.com/microsoft/mu_basecore.git "${MU_BASECORE_SHA}"
fetch_pinned Common/Mu https://github.com/microsoft/mu_plus.git "${MU_SHA}"
fetch_pinned Common/Mu_OEM_Sample https://github.com/microsoft/mu_oem_sample.git "${MU_OEM_SHA}"
fetch_pinned Silicium-ACPI https://github.com/Project-Silicium/Silicium-ACPI.git "${SILICIUM_ACPI_SHA}"
fetch_pinned Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl \
  https://github.com/openssl/openssl.git "${OPENSSL_SHA}"

python3 build_uefi.py -d mro -r "${TARGET}" -c
