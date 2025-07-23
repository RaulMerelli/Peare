namespace PeareModule
{
    public static class RT_DLGINCLUDE
    {
        public static unsafe string Get(byte[] data)
        {
            int offset = 0;
            return RT_STRING.ReadNullTerminatedString(data, ref offset, 850);
        }
    }
}
