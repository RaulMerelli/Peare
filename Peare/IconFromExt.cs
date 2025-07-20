using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Peare
{
    public class IconFromExt
    {
        const uint SHGFI_ICON = 0x000000100;
        const uint SHGFI_SMALLICON = 0x000000001;
        const uint SHGFI_USEFILEATTRIBUTES = 0x000000010;
        const uint FILE_ATTRIBUTE_DIRECTORY = 0x10;
        const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr SHGetFileInfo(string pszPath, uint dwFileAttributes, ref SHFILEINFO psfi, uint cbFileInfo, uint uFlags);

        [DllImport("user32.dll", SetLastError = true)]
        static extern bool DestroyIcon(IntPtr hIcon);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct SHFILEINFO
        {
            public IntPtr hIcon;
            public int iIcon;
            public uint dwAttributes;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string szDisplayName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)]
            public string szTypeName;
        }

        public static Bitmap Get(string extension)
        {
            SHFILEINFO shinfo = new SHFILEINFO();
            IntPtr hImg = SHGetFileInfo(extension, FILE_ATTRIBUTE_NORMAL, ref shinfo, (uint)Marshal.SizeOf(shinfo),
                SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

            Bitmap bitmap = null;
            if (shinfo.hIcon != IntPtr.Zero)
            {
                using (Icon icon = Icon.FromHandle(shinfo.hIcon))
                {
                    bitmap = icon.ToBitmap();
                }
                DestroyIcon(shinfo.hIcon);
            }
            return bitmap;
        }

        public static Bitmap GetFolder()
        {
            SHFILEINFO shinfo = new SHFILEINFO();
            IntPtr hImg = SHGetFileInfo(
                "folder", // any string, as FILE_ATTRIBUTE_DIRECTORY is set
                FILE_ATTRIBUTE_DIRECTORY,
                ref shinfo,
                (uint)Marshal.SizeOf(shinfo),
                SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES
            );

            Bitmap bitmap = null;
            if (shinfo.hIcon != IntPtr.Zero)
            {
                using (Icon icon = Icon.FromHandle(shinfo.hIcon))
                {
                    bitmap = icon.ToBitmap();
                }
                DestroyIcon(shinfo.hIcon);
            }
            return bitmap;
        }

    }
}
