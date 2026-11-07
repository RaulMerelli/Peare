# XBCOMPRESS validation samples

The ABI test uses two local samples made by Microsoft's independent
`xbcompress` utility from the installed Windows `cmd.exe` file. The utility is
built from Microsoft's Xbox GDK sample source. The samples are not stored in
this repository because the input executable is a Windows component.

The LZMS sample uses algorithm 5. The MSZIP sample uses algorithm 2 and spans
multiple 32 KiB blocks. Microsoft's utility also decoded both outputs during
sample validation. The expected payload is the unchanged `cmd.exe` input.

Keep the generated files outside the repository and set these variables before
running the ABI test:

```powershell
$samples = "C:\path\to\xbcompress\samples"
$env:PEARE_XBCOMPRESS_LZMS_SAMPLE = "$samples\lzms\cmd.ex_"
$env:PEARE_XBCOMPRESS_MSZIP_SAMPLE = "$samples\mszip\cmd.ex_"
$env:PEARE_XBCOMPRESS_INPUT = "$samples\cmd.exe"
python -m pytest -q tests\test_xbcompress_samples_abi.py
```

An additional local LZMS sample covers multiple blocks. It uses the installed
Windows `ntdll.dll` file and the same Microsoft utility:

```powershell
$env:PEARE_XBCOMPRESS_LZMS_MULTIBLOCK_SAMPLE = "$samples\lzms-multiblock\ntdll.dl_"
$env:PEARE_XBCOMPRESS_LZMS_MULTIBLOCK_INPUT = "$samples\lzms-multiblock\ntdll.dll"
```

The GUI preview runner can open either sample without an internal format-specific
test path:

```powershell
python tests\run_gui_preview_tests.py --exe build-windows\Release\Peare.exe `
  --case xbcompress-lzms "$samples\lzms\cmd.ex_" "" `
  --case xbcompress-mszip "$samples\mszip\cmd.ex_" ""
```
