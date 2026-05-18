// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <https://www.gnu.org/licenses/>.

#ifndef VILLAGESQL_DETAIL_ABI_SIGNATURE_LITERALS_H
#define VILLAGESQL_DETAIL_ABI_SIGNATURE_LITERALS_H

// Lightweight pin-literal API used by extensions.  This header is the
// extension-facing half of the ABI fingerprinting system: it defines
// the fingerprint string format and provides a per-target literal
// selector.  Computing fingerprints requires Boost.PFR-driven
// structural walking, which lives in abi_signature_compute.h
// (server-side only).
//
// Drift between extension and server is caught at extension load time
// by strcmp on the wire (vef_required_capability_t::vtable_hash).
// Structural correctness on the server side is guaranteed by
// abi_signature_compute.h's VEF_PIN_VERIFY, which static_asserts each
// pin literal against the structurally-computed hash at server
// compile time.
//
// The pin literal format is "hash-XXXXXXXXXXXXXXXX" (5-char tag +
// 16 lowercase hex digits, NUL-terminated -> 22 bytes including NUL).
// The empty string "" is the sentinel for "no pin available" -- used
// transiently for capabilities whose per-target literals have not
// been recorded yet (run the abi_pin_literals gunit test to get
// them).  An empty pin matches another empty pin at load time but
// not a real pin, which is intentional: an extension built against
// an unpinned SDK header can only load into a server that is also
// running with an empty pin for the same capability.

#include <array>
#include <cstddef>
#include <string_view>

namespace villagesql::detail {

// Textual fingerprint: "verhash-" + 3-digit zero-padded ABI version +
// "-" + 16 lowercase hex chars + trailing NUL.  Example:
//   "verhash-001-d51bd5e5b09abd0a"
//
// The "verhash-" prefix is a *scheme tag*.  Every fingerprint produced
// by this scheme begins with it, and matching is by strcmp, so the tag
// travels on the wire alongside the digest.  This means the hashing
// rules can be upgraded later by switching to a new prefix (e.g.
// "verhash2-...") without disturbing already-deployed extensions: the
// server can register both old and new vtables under the same
// capability name with their respective prefixed digests, or refuse
// extensions whose prefix it no longer supports.
//
// The 3-digit version captures the *intentional* ABI version a
// capability declares.  Together with the 16-hex structural digest, it
// gives two ways to flag drift:
//
//   * Changing a struct's shape (without bumping the version) flips
//     the hex digest -> pin mismatch.
//   * Bumping the version (without any shape change) flips the
//     version digits -> pin mismatch.
//
// Both flow through the same strcmp at extension load time.
struct AbiFingerprint {
  static constexpr std::size_t kVerPrefixLen = 8;  // "verhash-"
  static constexpr std::size_t kVersionLen = 3;    // "000".."999"
  static constexpr std::size_t kSepLen = 1;        // "-"
  static constexpr std::size_t kHexLen = 16;
  static constexpr std::size_t kLen =
      kVerPrefixLen + kVersionLen + kSepLen + kHexLen;  // 28
  static constexpr std::size_t kBufSize = kLen + 1;     // 29 with NUL

  std::array<char, kBufSize> chars{};

  constexpr std::string_view view() const {
    return std::string_view(chars.data(), kLen);
  }
  constexpr const char *c_str() const { return chars.data(); }

  constexpr bool operator==(const AbiFingerprint &other) const {
    for (std::size_t i = 0; i < kLen; ++i) {
      if (chars[i] != other.chars[i]) return false;
    }
    return true;
  }
  constexpr bool operator!=(const AbiFingerprint &other) const {
    return !(*this == other);
  }

  // Build a fingerprint from a 28-char string literal (plus the
  // compiler's implicit NUL = 29 bytes total) of the form
  // "verhash-NNN-XXXXXXXXXXXXXXXX".  Used by the verification path in
  // abi_signature_compute.h to compare a pinned literal against the
  // structurally-computed fingerprint.
  template <std::size_t N>
  static constexpr AbiFingerprint from_literal(const char (&literal)[N]) {
    static_assert(N == kBufSize,
                  "AbiFingerprint literal must be exactly 28 characters "
                  "(\"verhash-\" + 3-digit version + \"-\" + 16 hex digits)");
    AbiFingerprint fp{};
    for (std::size_t i = 0; i < kLen; ++i) fp.chars[i] = literal[i];
    fp.chars[kLen] = '\0';
    return fp;
  }
};

}  // namespace villagesql::detail

// VEF_PIN(mac_lit, x86_lit, arm_lit) -- pure platform-selector macro.
// Expands to the pin literal for the current build target.  Used by
// extensions (in CapabilityTraits<...>::kVtableHash and kCapabilityConfigHash)
// and by the server-side VEF_PIN_VERIFY chain.  Targets split by (OS,
// architecture) because most ABI divergence today lives along that
// axis.
//
// Apple is currently a single .mac() cell because Apple arm64 and Apple
// x86_64 share the C ABI for every primitive our preview ABIs touch.
// Linux is split between linux_x86 and linux_arm so divergence between
// Linux x86_64 and Linux arm64 surfaces if it ever appears.
//
// On an unrecognised target, expands to "" -- an empty pin, which
// won't match any real registered pin and so triggers a load-time
// rejection of the extension.  Add a new branch when porting to a new
// target.
//
// Example pin site (in a capability's _register.h):
//   static constexpr const char *kVtableHash =
//       VEF_PIN(VEF_PREVIEW_KEYRING_ABI_HASH_MAC,
//               VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_X86,
//               VEF_PREVIEW_KEYRING_ABI_HASH_LINUX_ARM);
//
// `linux` (bare) is reserved here: glibc-targeted GCC predefines
// `#define linux 1`, which would expand a name with `linux` in it
// surprisingly.  The macro arguments use explicit `linux_x86` /
// `linux_arm` everywhere.
//
// TODO(villagesql-windows): add windows_x86 / windows_arm slots when
// we have a Windows build to verify against.
#if defined(__APPLE__)
#define VEF_PIN(mac_lit, x86_lit, arm_lit) (mac_lit)
#elif defined(__linux__) && defined(__x86_64__)
#define VEF_PIN(mac_lit, x86_lit, arm_lit) (x86_lit)
#elif defined(__linux__) && defined(__aarch64__)
#define VEF_PIN(mac_lit, x86_lit, arm_lit) (arm_lit)
#else
#define VEF_PIN(mac_lit, x86_lit, arm_lit) ""
#endif

#endif  // VILLAGESQL_DETAIL_ABI_SIGNATURE_LITERALS_H
