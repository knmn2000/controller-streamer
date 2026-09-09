# Prebuilt Android sender

`controller-streamer-sender.apk` — install this on the phone that has your
controller paired to it, so you don't need the Android SDK to get started.

Verify it before installing:

```bash
sha256sum -c controller-streamer-sender.apk.sha256
```

```powershell
Get-FileHash controller-streamer-sender.apk -Algorithm SHA256
```

Then copy it to the phone and tap it. Android will ask you to allow installing
from an unknown source; that is expected for a sideloaded app.

After installing, open the app and tap **Enable background capture** — without
that, input stops the moment you leave the app or lock the phone. See the
[main README](../README.md) for the full setup.

## About the signature

This APK is **debug-signed** with a throwaway key whose password is published in
`sender-android/build-apk.cmd`. That is fine for sideloading and it is what
lets the build work with no key management, but it has two consequences worth
knowing:

- It is **not** a release signature. Do not treat it as proof of origin. If you
  care, build from source — that is the point of the licence.
- The signing key is generated locally and is **not** in the repository, so an
  APK you build yourself will have a *different* signature and Android will
  refuse to install it over this one. Uninstall first, or keep using one or the
  other.

For a proper release you would generate your own key, keep it out of the repo,
and attach the signed APK to a GitHub Release rather than committing it.
