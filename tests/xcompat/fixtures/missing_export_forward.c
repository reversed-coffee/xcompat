#pragma comment(linker, "/export:Missing=leaf.Absent,@1")
void xcompat_missing_export_fixture(void) {}
void* xcompat_missing_export_relocation = (void*)&xcompat_missing_export_fixture;
