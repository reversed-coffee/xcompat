#pragma comment(linker, "/export:Cycle=forward_cycle_a.Cycle")
void xcompat_forward_cycle_b_fixture(void) {}
void* xcompat_forward_cycle_b_relocation = (void*)&xcompat_forward_cycle_b_fixture;
