# Run Peare

## Windows

Download and extract the portable Windows ZIP. Run `Peare.exe` from the extracted
folder. Use **Settings > File associations...** to manage file defaults.
Windows opens its Default Apps page. Confirm each default there. Peare does not
edit **Open with** lists.

## Linux

Download and extract the Linux archive. Run the `Peare` executable from the
extracted folder. Use **File associations** to set per-user file defaults.
Peare does not add entries to the **Added Associations** list.

## macOS

Open the Peare application. Use **File associations** to set per-user file
defaults. Peare restores a previous handler when one was saved.

## Android

Download the ARM64 APK and install it on a compatible device. The CI APK uses a
debug signing key. It is for local use and is not a store release.

The project does not yet verify runtime behavior on a physical Android device.
Android controls file handlers through the system file picker. Peare cannot
assign itself as the default handler from its settings.
