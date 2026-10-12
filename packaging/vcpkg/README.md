# vcpkg port for mcpp

Overlay port + PR-ready sources for [microsoft/vcpkg](https://github.com/microsoft/vcpkg).

## Use as an overlay (today, no PR needed)

```bash
git clone https://github.com/zhengqiuyang/mcpp
cmake -B build -S yourproject \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_OVERLAY_PORTS=<mcpp-clone>/packaging/vcpkg
# then in CMakeLists.txt: find_package(mcpp 0.4 REQUIRED)
```

## Submit to the official registry

1. Fork microsoft/vcpkg; copy `mcpp/` from this directory into `ports/mcpp/`.
2. Verify: `<vcpkg>/vcpkg install mcpp` (it will fail with the correct SHA512
   if the tarball hash drifted — update `portfile.cmake` accordingly).
3. `vcpkg x-add-version mcpp` to regenerate `versions/m-/mcpp.json`.
4. Open the PR titled `[mcpp] Add version 0.4.0`.

## Notes

- Pinned to the **v0.3.0** tarball hash (the last published release); bump
  REF/SHA512/version together when v0.4.0 ships — the port only installs the
  header-only library + CMake config from `mcpp/`.
