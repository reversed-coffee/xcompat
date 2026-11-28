#pragma comment(linker, "/export:Missing=leaf.Absent")
void xcompat_missing_export_fixture(void) {}
void* xcompat_missing_export_relocation = (void*)&xcompat_missing_export_fixture;
