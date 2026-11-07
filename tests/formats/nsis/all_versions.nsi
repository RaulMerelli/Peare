Name "NSIS Version Validation"
OutFile "nsis-version-validation.exe"
InstallDir "$TEMP\Peare NSIS validation"
Section "Payload"
SetOutPath "$INSTDIR"
File "payload\README.md"
File "payload\SUPPORTED_FORMATS.md"
SectionEnd
