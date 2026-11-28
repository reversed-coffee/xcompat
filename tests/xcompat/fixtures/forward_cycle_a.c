#pragma comment(linker, "/export:Cycle=forward_cycle_b.Cycle")
void xcompat_forward_cycle_a_fixture(void) {}
void* xcompat_forward_cycle_a_relocation = (void*)&xcompat_forward_cycle_a_fixture;
