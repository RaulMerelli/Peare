#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build-windows\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\build-windows\installer"
#endif

[Setup]
AppId={{3D7C4B3A-C5CA-4B4E-9EAF-AE1C7784F02B}
AppName=Peare
AppVersion={#AppVersion}
AppPublisher=Peare
DefaultDirName={localappdata}\Programs\Peare
DefaultGroupName=Peare
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
ChangesAssociations=yes
SetupIconFile=icons\archive.ico
UninstallDisplayIcon={app}\Peare.exe
OutputDir={#OutputDir}
OutputBaseFilename=Peare-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "associations"; Description: "Add Peare to Open with for supported formats"; Flags: checkedonce
Name: "contextmenu"; Description: "Add Open with Peare to file and folder menus"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "icons\*.ico"; DestDir: "{app}\icons"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Peare"; Filename: "{app}\Peare.exe"
Name: "{autodesktop}\Peare"; Filename: "{app}\Peare.exe"; Tasks: desktopicon

[Code]
const
  ArchiveExtensions = 'zip;zipx;7z;rar;chm;jar;war;ear;aar;apk;apks;aab;epub;docx;xlsx;pptx;odt;ods;odp;ipa;ipsw;xpi;nupkg;snupkg;whl;vsix;cbz;kmz;zap14;zap15;tar;wpi;rpm;cab;asar;gz;gzip;tgz;xz;txz;lzma;pack;pack2;bz2;bzip2;tbz;tbz2;z;taz;lz;tlz;zst;ar;a;arj;xar;cpio;deb;udeb;lzh;lha;wad;msi;msm;msp;mst;pcp;cub;uue;uu;sz;kwj;appx;msix;appxbundle;eappx;emsix;emsixbundle;xap;bkf;mtf;wim;ffu;sdi;pup';
  DiskExtensions = 'vmdk;vhd;vhdx;vdi;qcow;qcow2;img;raw;xva;hdd;hfs';
  OpticalExtensions = 'iso;bin;cue;mdf;mds;dmg;ccd;udf';
  FloppyExtensions = 'ima;vfd;dsk;imz';
  ImageExtensions = 'png;jpg;jpeg;pcx;pcc;gif;bmp;tif;tiff;ico;cur;wmf;emf';
  FontExtensions = 'fnt;fon;font;ttf;ttc;otf;dfont;woff;woff2;eot;bdf;pcf;snf;dwf;afm;pfm;pfa;pfb;type1;ofm;shp;shx;sfn;psf;pk;vfont;fb;bmf';
  TextExtensions = 'txt;log;cfg;ini;xml;json;yaml;yml;csv;md;asm;c;cc;cpp;h;hpp;py;ps1;bat;cmd;sh;sql;html;css;js;ts;hex;ihex;resx;exe;dll;ocx;sys;efi;bcd;hiv;reg';

procedure RegisterExtensionList(const Extensions, ProgId: String);
var
  Remaining, Extension: String;
  Separator: Integer;
begin
  Remaining := Extensions;
  while Remaining <> '' do begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then begin
      Extension := Remaining;
      Remaining := '';
    end else begin
      Extension := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    RegWriteStringValue(HKCU, 'Software\Classes\.' + Extension + '\OpenWithProgids', ProgId, '');
    RegWriteStringValue(HKCU, 'Software\Peare\Capabilities\FileAssociations',
      '.' + Extension, ProgId);
  end;
end;

procedure RemoveExtensionList(const Extensions, ProgId: String);
var
  Remaining, Extension: String;
  Separator: Integer;
begin
  Remaining := Extensions;
  while Remaining <> '' do begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then begin
      Extension := Remaining;
      Remaining := '';
    end else begin
      Extension := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    RegDeleteValue(HKCU, 'Software\Classes\.' + Extension + '\OpenWithProgids', ProgId);
    RegDeleteValue(HKCU, 'Software\Peare\Capabilities\FileAssociations', '.' + Extension);
  end;
end;

procedure RegisterProgId(const ProgId, Description, IconFile: String);
var
  ApplicationPath: String;
begin
  ApplicationPath := '"' + ExpandConstant('{app}\Peare.exe') + '" "%1"';
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId, '', Description);
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId + '\DefaultIcon', '',
    ExpandConstant('{app}\icons\' + IconFile));
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId + '\shell\open\command', '', ApplicationPath);
end;

procedure RegisterContextVerb(const KeyPath, TargetArgument: String);
begin
  RegWriteStringValue(HKCU, 'Software\Classes\' + KeyPath, '', 'Open with Peare');
  RegWriteStringValue(HKCU, 'Software\Classes\' + KeyPath, 'Icon',
    ExpandConstant('{app}\Peare.exe'));
  RegWriteStringValue(HKCU, 'Software\Classes\' + KeyPath + '\command', '',
    '"' + ExpandConstant('{app}\Peare.exe') + '" "' + TargetArgument + '"');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('associations') then begin
      RegWriteStringValue(HKCU, 'Software\Peare\Capabilities', 'ApplicationName', 'Peare');
      RegWriteStringValue(HKCU, 'Software\Peare\Capabilities', 'ApplicationDescription',
        'Open archives, resources, and disk images with Peare.');
      RegWriteStringValue(HKCU, 'Software\RegisteredApplications', 'Peare',
        'Software\Peare\Capabilities');
      RegisterProgId('Peare.Archive', 'Peare archive', 'archive.ico');
      RegisterProgId('Peare.DiskImage', 'Peare disk image', 'disk.ico');
      RegisterProgId('Peare.OpticalImage', 'Peare optical image', 'optical.ico');
      RegisterProgId('Peare.FloppyImage', 'Peare floppy image', 'floppy.ico');
      RegisterProgId('Peare.Image', 'Peare image', 'image.ico');
      RegisterProgId('Peare.Font', 'Peare font', 'font.ico');
      RegisterProgId('Peare.Text', 'Peare text or source file', 'text.ico');
      RegisterExtensionList(ArchiveExtensions, 'Peare.Archive');
      RegisterExtensionList(DiskExtensions, 'Peare.DiskImage');
      RegisterExtensionList(OpticalExtensions, 'Peare.OpticalImage');
      RegisterExtensionList(FloppyExtensions, 'Peare.FloppyImage');
      RegisterExtensionList(ImageExtensions, 'Peare.Image');
      RegisterExtensionList(FontExtensions, 'Peare.Font');
      RegisterExtensionList(TextExtensions, 'Peare.Text');
    end;
    if WizardIsTaskSelected('contextmenu') then begin
      RegisterContextVerb('*\shell\Peare', '%1');
      RegisterContextVerb('Directory\shell\Peare', '%1');
      RegisterContextVerb('Directory\Background\shell\Peare', '%V');
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RemoveExtensionList(ArchiveExtensions, 'Peare.Archive');
    RemoveExtensionList(DiskExtensions, 'Peare.DiskImage');
    RemoveExtensionList(OpticalExtensions, 'Peare.OpticalImage');
    RemoveExtensionList(FloppyExtensions, 'Peare.FloppyImage');
    RemoveExtensionList(ImageExtensions, 'Peare.Image');
    RemoveExtensionList(FontExtensions, 'Peare.Font');
    RemoveExtensionList(TextExtensions, 'Peare.Text');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Peare\Capabilities');
    RegDeleteValue(HKCU, 'Software\RegisteredApplications', 'Peare');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.Archive');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.DiskImage');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.OpticalImage');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.FloppyImage');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.Image');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.Font');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Peare.Text');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\*\shell\Peare');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\shell\Peare');
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\Directory\Background\shell\Peare');
  end;
end;
