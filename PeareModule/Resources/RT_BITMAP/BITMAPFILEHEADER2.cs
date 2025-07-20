using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct BITMAPFILEHEADER2
    {
        public ushort usType;
        public uint cbSize;
        public short xHotspot;
        public short yHotspot;
        public uint offBits;
        // BITMAPINFOHEADER2 bmp2; // This is a variable-length field in the spec, handled by offset
    }
}
