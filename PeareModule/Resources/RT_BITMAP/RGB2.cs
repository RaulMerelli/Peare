using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct RGB2
    {
        public byte bBlue;
        public byte bGreen;
        public byte bRed;
        public byte fcOptions;
    }
}
