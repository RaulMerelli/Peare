namespace PeareModule
{
    public static class RT_DIALOG
    {
        public static string Get(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            if ((properties.headerType == ModuleResources.HeaderType.LE && properties.versionType == ModuleResources.VersionType.OS2) ||
                (properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                properties.headerType == ModuleResources.HeaderType.LX)
            {
                // Structure is different for OS/2
                return OS2_RT_DIALOG.Get(resData);
            }
            return "";
        }
    }
}
