# Install Peare

## Windows

Download the Windows installer from the download button in the project README.
Run the installer. It installs Peare for the current Windows user and does not
need administrator access.

The installer adds Peare to the Windows Open with list for supported file types.
It does not replace an existing default application. Use Windows Settings to
select Peare as the default application for a file type.

The installer can also add Open with Peare to file and folder context menus.
You can clear these options during setup. The uninstaller removes Peare's
context-menu entries and file-type registrations.

The portable Windows ZIP contains the application and its runtime files. Extract
it to a writable folder, then run `Peare.exe`.

## Linux

Download and extract the Linux archive. Run the `Peare` executable from the
extracted folder.

## Android

Download the ARM64 APK and install it on a compatible device. The CI APK uses a
debug signing key. It is for local use and is not a store release.

The project does not yet verify runtime behavior on a physical Android device.
