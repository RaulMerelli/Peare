using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct BITMAPINFOHEADER2
    {
        public uint cbFix;
        public uint cx;
        public uint cy;
        public ushort cPlanes;
        public ushort cBitCount;
        public uint ulCompression;
        public uint cbImage;
        public uint cxResolution;
        public uint cyResolution;
        public uint cclrUsed;
        public uint cclrImportant;
        public ushort usUnits;
        public ushort usReserved;
        public ushort usRecording;
        public ushort usRendering;
        public uint cSize1;
        public uint cSize2;
        public uint ulColorEncoding;
        public uint ulIdentifier;
    }
}
