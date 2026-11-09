# Build and release

The supported package is one public Windows 11 x64 MSI using standard WiX UI. Public release is
not yet validated; see docs/release-readiness.md. Kernel code is never built.

Prerequisites: Visual Studio C++/MFC x64 tools, Windows SDK (including signtool),
.NET SDK, Node.js 22.18 or newer with npm, PowerShell 7 (pwsh), WiX 5.0.2 CLI and matching UI extension. No WDK or ARM64 tools required.

```powershell
.\build.ps1 Ci --configuration Release --platform x64 --compiler-path "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
```

Ci builds applications and tests, executes native/managed tests, acquires/verifies
the pinned signed driver and produces the public MSI. Editor dependencies are
locked in Editor/package-lock.json; staging runs npm ci, TypeScript/Vite build,
model tests and Electron packaging. Node/npm and dependency downloads are build
requirements only; the installed editor bundles its runtime and works offline. Use --driver-payload and
--sign-tool for explicit local verified inputs/tool paths.
HIDHIDE_DRIVER_PAYLOAD remains a supported equivalent.
Keep all recovery evidence in artifacts; never clear the entire directory.

Release uses a new output directory and already-built version-consistent apps:

```powershell
.\release.ps1 -Staging .\artifacts\staging\x64 -DriverPayload .\artifacts\unified-driver-verified -Out .\artifacts\release-candidate -NoSigning -SignTool <signtool.exe>
```

The staging directory must contain both native executables and the complete
Editor subdirectory produced by unified Ci. Desktop acceptance runs separately:
`cd Editor; npm run test:ui; node tests/editor-regressions.mjs; node tests/native-integration.mjs; node tests/launch-order.mjs; node tests/recent-mask.mjs`. These tests use
isolated fixtures and require an interactive desktop; they do not operate the
installed driver. See docs/electron-editor.md.

The current release decision is explicitly unsigned owned setup/application code.
This never means unsigned kernel code: catalog trust/membership and exact driver
hashes are mandatory. Optional signing omits -NoSigning and supplies -CertName.
Only owned code is signed; the upstream driver is never re-signed or regenerated.
The pipeline signs owned code before packaging and then the MSI using WiX's
signing sequence. No upload happens automatically.

Inspect release-manifest.json, logs and test XML, then validate the exact final
artifact. A successful build does not close native upgrade, recovery, offline,
physical-device or alternate-user acceptance gates. Dirty source evidence must
not be represented as an exact clean source commit.

Historical companion-only MSI build commands are superseded.
