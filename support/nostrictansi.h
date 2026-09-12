#undef __STRICT_ANSI__
#ifndef asm
#define asm __asm__
#endif

#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif
int closesocket(int socket);
#ifdef __cplusplus
}
#endif
#endif



