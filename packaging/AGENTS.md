# Packaging (`packaging/`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is.

- macOS `.dmg` is signed (hardened runtime) by `packaging/macos/sign_app.sh`
  from CPack (`packaging/macos/cpack_sign.cmake`, identity in
  `PYCHRON_CODESIGN_IDENTITY`, `-` for ad hoc), then signed, notarized and
  stapled in `release.yml` when signing secrets are set
  (`docs/installation_runbook.md` section 1.4). Something the programs newly
  need under hardened runtime (a device, a kind of library) is an
  entitlement in `packaging/macos/entitlements.plist`.
- `packaging/sim`: `elctl sim serve` in a Linux image. Not in CI, so a change
  to `elctl`'s build options or to `sim serve`'s flags is tried there by hand
  (`README.md` beside it). Published ports stay on host loopback by default:
  emulators have no authentication.
