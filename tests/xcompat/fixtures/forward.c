#pragma comment(linker, "/export:ForwardNamed=provider.NamedValue,@1")
#pragma comment(linker, "/export:ForwardOrdinal=provider.#2,@2")
void xcompat_forward_fixture(void) {}
void* xcompat_forward_relocation = (void*)&xcompat_forward_fixture;
