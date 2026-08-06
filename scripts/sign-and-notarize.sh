#!/usr/bin/env bash
set -euo pipefail

###############################################################################
# macOS Code Signing & Notarization for Logos CLI tools (flat dir layout)
#
# Signs and notarizes a plain bin/lib/modules directory (nix cli-bundle-dir
# output) instead of an .app bundle. Because there is no bundle, there is
# also no stapling - Gatekeeper verifies the notarization ticket online.
#
# Supports modes:
#   --mode sign      - Sign only
#   --mode notarize  - Notarize only (requires pre-signed dir)
#   --mode both      - Sign and notarize (default)
#
# Usage:
#   ./scripts/sign-and-notarize.sh --dir PATH [--output PATH] [--mode MODE] [--timeout TIMEOUT]
#
# Required env vars:
#   MACOS_CODESIGN_IDENT       - Developer ID Application identity
#   MACOS_NOTARY_ISSUER_ID     - App Store Connect API issuer ID
#   MACOS_NOTARY_KEY_ID        - App Store Connect API key ID
#   MACOS_NOTARY_KEY_FILE      - Path to .p8 AuthKey file
#   MACOS_KEYCHAIN_PASS        - Password for the .p12 certificate
#   MACOS_KEYCHAIN_FILE        - Path to the .p12 certificate file
###############################################################################

# Needed since Apple time server can sometimes timeout
codesign_with_retry() {
    local attempts=3
    local delay=15
    local i=1
    while (( i <= attempts )); do
        if codesign "$@"; then
            return 0
        fi
        echo "  codesign attempt ${i}/${attempts} failed, retrying in ${delay}s..."
        sleep "$delay"
        (( i++ ))
    done
    echo "ERROR: codesign failed after ${attempts} attempts"
    return 1
}

DIR_PATH=""
OUTPUT_PATH=""
MODE="both"
TIMEOUT="30m"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR_PATH="$2"
      shift 2
      ;;
    --output)
      OUTPUT_PATH="$2"
      shift 2
      ;;
    --mode)
      MODE="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT="$2"
      shift 2
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

# Validate mode
if [[ ! "$MODE" =~ ^(sign|notarize|both)$ ]]; then
  echo "Error: --mode must be 'sign', 'notarize', or 'both'" >&2
  exit 1
fi

[[ -n "$DIR_PATH" ]] || { echo "Error: --dir is required" >&2; exit 1; }
[[ -d "$DIR_PATH" ]] || { echo "Error: Directory not found: $DIR_PATH" >&2; exit 1; }

TEMP_DIR=$(mktemp -d)
CERTS_DIR="${TEMP_DIR}/certs"
mkdir -p "${CERTS_DIR}"
trap "rm -rf '${TEMP_DIR}'" EXIT

WORK_DIR="${TEMP_DIR}/bundle-dir"
KEYCHAIN_NAME="build.keychain"
KEYCHAIN_DB_PATH="${HOME}/Library/Keychains/${KEYCHAIN_NAME}-db"

# Codesign options - hardened runtime required for notarization
CODESIGN_OPTS=(
    --force
    --timestamp
    --options runtime
    --keychain "${KEYCHAIN_DB_PATH}"
    --sign "${MACOS_CODESIGN_IDENT}"
)

echo "Copying directory to writable temp location."
# DIR_PATH may be a symlink (nix `result`), so resolve it via `/.` to copy
# the tree contents instead of the symlink itself. Symlinks *inside* the
# tree (framework Versions/, versioned dylib aliases) are preserved.
mkdir -p "$WORK_DIR"
cp -a "${DIR_PATH}/." "$WORK_DIR/"
chmod -R u+w "$WORK_DIR"

# Returns 0 if file is a Mach-O binary (any flavor)
is_macho() {
    file -b "$1" 2>/dev/null | grep -q 'Mach-O'
}

###############################################################################
# SIGN MODE
###############################################################################
if [[ "$MODE" =~ ^(sign|both)$ ]]; then
  echo "Starting signing phase."

  ###############################################################################
  # 1. Remove all existing signatures
  ###############################################################################
  echo "Removing existing signatures..."
  find "${WORK_DIR}" -name "_CodeSignature" -exec rm -rf {} + 2>/dev/null || true

  ###############################################################################
  # 2. Set up a temporary keychain and import the certificate
  ###############################################################################
  echo "Setting up keychain at ${KEYCHAIN_DB_PATH}"
  security delete-keychain "${KEYCHAIN_DB_PATH}" 2>/dev/null || true
  security create-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_DB_PATH}"
  security unlock-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_DB_PATH}"
  security set-keychain-settings -lut 21600 "${KEYCHAIN_DB_PATH}"

  ###############################################################################
  # 3. Import Trust Chain from Apple
  ###############################################################################
  echo "Downloading Apple Trust Chain."
  curl -fsSL -o "${CERTS_DIR}/AppleRootCA-G2.cer"  https://www.apple.com/certificateauthority/AppleRootCA-G2.cer
  curl -fsSL -o "${CERTS_DIR}/AppleWWDRCAG2.cer"   https://www.apple.com/certificateauthority/AppleWWDRCAG2.cer
  curl -fsSL -o "${CERTS_DIR}/DeveloperIDG2CA.cer" https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer

  echo "Importing Apple Trust Chain from local storage."
  for cert in \
      "${CERTS_DIR}/AppleRootCA-G2.cer" \
      "${CERTS_DIR}/AppleWWDRCAG2.cer" \
      "${CERTS_DIR}/DeveloperIDG2CA.cer"; do
      cert_name=$(basename "${cert}")
      if output=$(security import "${cert}" -k "${KEYCHAIN_DB_PATH}" -t cert 2>&1); then
          echo "  Imported: ${cert_name}"
      elif echo "${output}" | grep -q "already exists"; then
          echo "  Skipped (already in trust store): ${cert_name}"
      else
          echo "  ERROR importing ${cert_name}: ${output}"
          exit 1
      fi
  done

  # Ensure the system looks at our build keychain first
  security list-keychains -d user -s "${KEYCHAIN_DB_PATH}" /Library/Keychains/System.keychain

  ###############################################################################
  # 4. Import Identity and Set Permissions
  ###############################################################################
  echo "Extracting identity from P12."
  openssl pkcs12 -in "${MACOS_KEYCHAIN_FILE}" \
      -passin pass:"${MACOS_KEYCHAIN_PASS}" \
      -nodes -out "${TEMP_DIR}/cert_and_key.pem"

  echo "Importing identity from PEM."
  security import "${TEMP_DIR}/cert_and_key.pem" \
      -k "${KEYCHAIN_DB_PATH}" \
      -T /usr/bin/codesign

  # Allow codesign to access the key without UI prompts (required on Sequoia)
  security set-key-partition-list \
      -S apple-tool:,apple:,codesign: \
      -s -k "${MACOS_KEYCHAIN_PASS}" \
      "${KEYCHAIN_DB_PATH}"

  echo "Debug: Keychain contents"
  echo "Available identities in ${KEYCHAIN_DB_PATH}:"
  security find-identity -v "${KEYCHAIN_DB_PATH}" || echo "No identities found"

  echo "Re-unlocking keychain before signing."
  security unlock-keychain -p "${MACOS_KEYCHAIN_PASS}" "${KEYCHAIN_DB_PATH}"

  ###############################################################################
  # 5. Sign all dylibs (lib/, modules/, and anything nested below)
  ###############################################################################
  echo "Signing dylibs."
  while IFS= read -r dylib; do
      [[ -n "$dylib" ]] || continue
      echo "  Signing: ${dylib}"
      codesign_with_retry "${CODESIGN_OPTS[@]}" "$dylib" \
          || { echo "ERROR: failed to sign ${dylib}"; exit 1; }
  done < <(find "${WORK_DIR}" \
      -name '*.dylib' -type f \
      -not -path '*.framework/*' 2>/dev/null | sort)

  ###############################################################################
  # 6. Sign Qt frameworks (binary first, then bundle - order within fw matters)
  ###############################################################################
  echo "Signing Qt frameworks."
  while IFS= read -r fw; do
      fw_name=$(basename "${fw}" .framework)
      fw_binary="${fw}/Versions/A/${fw_name}"

      if [[ -f "${fw_binary}" ]]; then
          echo "  Signing framework binary: ${fw_binary}"
          codesign_with_retry "${CODESIGN_OPTS[@]}" "${fw_binary}" \
              || { echo "ERROR: failed to sign ${fw_binary}"; exit 1; }
      fi

      # Signing the framework *bundle* requires a valid bundle structure
      # (Resources/Info.plist). nix-bundle-dir ships minimal frameworks
      # without it, in which case the signed inner binary is sufficient --
      # notarization and dyld validate Mach-O signatures, not bundle seals.
      if [[ -f "${fw}/Resources/Info.plist" || -f "${fw}/Versions/A/Resources/Info.plist" ]]; then
          echo "  Signing framework bundle: ${fw}"
          codesign_with_retry "${CODESIGN_OPTS[@]}" "${fw}" \
              || { echo "ERROR: failed to sign ${fw}"; exit 1; }
      else
          echo "  Skipping bundle seal (no Info.plist, binary already signed): ${fw}"
      fi
  done < <(find "${WORK_DIR}/lib" -name '*.framework' -type d -maxdepth 1 2>/dev/null | sort)

  ###############################################################################
  # 7. Sign executables in bin/
  ###############################################################################
  # NOTE: no -perm filter -- GNU find (nix shells) rejects BSD's `-perm +111`
  # and the error would be silently swallowed, skipping all executables.
  # is_macho already filters out non-binaries (qt.conf etc).
  echo "Signing executables."
  SIGNED_EXE_COUNT=0
  while IFS= read -r exe; do
      [[ -n "$exe" ]] || continue
      is_macho "$exe" || continue
      echo "  Signing: ${exe}"
      codesign_with_retry "${CODESIGN_OPTS[@]}" "$exe" \
          || { echo "ERROR: failed to sign ${exe}"; exit 1; }
      (( SIGNED_EXE_COUNT++ )) || true
  done < <(find "${WORK_DIR}/bin" -type f 2>/dev/null | sort)

  if (( SIGNED_EXE_COUNT == 0 )); then
      echo "ERROR: no Mach-O executables found in ${WORK_DIR}/bin -- nothing was signed."
      exit 1
  fi

  ###############################################################################
  # 8. Verify (strict: must be OUR identity with hardened runtime,
  #    an ad-hoc nix signature must not pass silently)
  ###############################################################################
  echo "Verifying signatures."
  while IFS= read -r exe; do
      [[ -n "$exe" ]] || continue
      is_macho "$exe" || continue
      codesign --verify --strict --verbose=2 "$exe" \
          || { echo "ERROR: Signature verification failed for ${exe}."; exit 1; }
      siginfo=$(codesign -d --verbose=2 "$exe" 2>&1)
      echo "$siginfo" | grep -qF "Authority=${MACOS_CODESIGN_IDENT}" \
          || { echo "ERROR: ${exe} is not signed by '${MACOS_CODESIGN_IDENT}'."; exit 1; }
      echo "$siginfo" | grep -qE '^CodeDirectory .*flags=.*runtime' \
          || { echo "ERROR: ${exe} does not have hardened runtime enabled."; exit 1; }
  done < <(find "${WORK_DIR}/bin" -type f 2>/dev/null | sort)

  echo "Signing phase complete"
fi

###############################################################################
# NOTARIZE MODE
###############################################################################
if [[ "$MODE" =~ ^(notarize|both)$ ]]; then
  echo "Starting notarization phase."

  ###############################################################################
  # 9. Create ZIP for notarization
  ###############################################################################
  echo "Creating ZIP for notarization."
  NOTARIZE_ZIP="${TEMP_DIR}/notarize-$$.zip"
  ditto -c -k --keepParent "${WORK_DIR}" "${NOTARIZE_ZIP}"

  ###############################################################################
  # 10. Submit for notarization
  ###############################################################################
  # notarytool exits 0 even when the submission finishes as Invalid,
  # so we must parse the final status ourselves and fetch the log on failure.
  echo "Submitting for notarization."
  NOTARY_OUTPUT=$(xcrun notarytool submit "${NOTARIZE_ZIP}" \
      --issuer "${MACOS_NOTARY_ISSUER_ID}" \
      --key-id "${MACOS_NOTARY_KEY_ID}" \
      --key "${MACOS_NOTARY_KEY_FILE}" \
      --wait \
      --timeout "${TIMEOUT}" 2>&1) || { echo "${NOTARY_OUTPUT}"; echo "ERROR: notarytool submission failed."; exit 1; }
  echo "${NOTARY_OUTPUT}"

  if ! echo "${NOTARY_OUTPUT}" | grep -qE '^ *status: Accepted$'; then
      SUBMISSION_ID=$(echo "${NOTARY_OUTPUT}" | grep -m1 -oE 'id: [0-9a-f-]+' | awk '{print $2}')
      echo "ERROR: notarization was not Accepted."
      if [[ -n "${SUBMISSION_ID}" ]]; then
          echo "Fetching notarization log for submission ${SUBMISSION_ID}:"
          xcrun notarytool log "${SUBMISSION_ID}" \
              --issuer "${MACOS_NOTARY_ISSUER_ID}" \
              --key-id "${MACOS_NOTARY_KEY_ID}" \
              --key "${MACOS_NOTARY_KEY_FILE}" || true
      fi
      exit 1
  fi

  # NOTE: no stapling - tickets cannot be stapled to flat directories or
  # tarballs, only to .app/.dmg/.pkg. Gatekeeper fetches the ticket online.

  ###############################################################################
  # 11. Final verification (all bin/ executables, strict)
  ###############################################################################
  echo "Final verification."
  while IFS= read -r exe; do
      [[ -n "$exe" ]] || continue
      is_macho "$exe" || continue
      codesign --verify --strict --verbose=2 "$exe" \
          || { echo "ERROR: Final signature verification failed for ${exe}."; exit 1; }
      siginfo=$(codesign -d --verbose=2 "$exe" 2>&1)
      echo "$siginfo" | grep -qF "Authority=${MACOS_CODESIGN_IDENT}" \
          || { echo "ERROR: ${exe} is not signed by '${MACOS_CODESIGN_IDENT}'."; exit 1; }
      spctl --assess --type execute --verbose=2 "$exe" \
          || echo "WARNING: spctl assessment rejected for ${exe} (expected for non-quarantined CLI binaries, informational only)"
  done < <(find "${WORK_DIR}/bin" -type f 2>/dev/null | sort)

  security delete-keychain "${KEYCHAIN_DB_PATH}" 2>/dev/null || true

  echo "Notarization phase complete"
fi

###############################################################################
# Output
###############################################################################
if [[ -n "$OUTPUT_PATH" ]]; then
  rm -rf "$OUTPUT_PATH"
  cp -a "$WORK_DIR" "$OUTPUT_PATH"
  echo "Signed output: ${OUTPUT_PATH}"
fi