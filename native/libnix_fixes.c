/*
 * libnix_fixes.c - replacements for libnix routines that are wrong.
 *
 * Each function here is linked as an object ahead of the libnix archives, so
 * the linker binds ours and never pulls the archive member (every one of
 * these lives in its own member - checked with nm - so there is no duplicate
 * definition to fight).
 *
 * wmemcpy (diagnosed 2026-08-16, garbled fonts and menu text):
 *   libnix's version is
 *       move.l n,d0 ; add.l d0,d0 ; CopyMem(src, dst, d0)
 *   i.e. it copies n*2 bytes. wchar_t is 4 bytes on this toolchain
 *   (wmemset, wmemmove, wmemcmp, wmemchr and wcslen next to it all use *4).
 *   libstdc++ copies std::wstring through char_traits<wchar_t>::copy ==
 *   wmemcpy on every reallocation, so any wstring longer than the SSO buffer
 *   (3 wide chars) ends up with its second half uninitialised. In the game
 *   that meant Font's wchar_t->glyph map was keyed by garbage and every
 *   string to be drawn was garbage in the same way: letters rendered as
 *   glyphs from the last font image (Korean), text was unreadable.
 *
 * Kept deliberately small and boring; the game is not going to be limited by
 * how fast it copies wide strings.
 */
#include <stddef.h>
#include <wchar.h>

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n)
{
	wchar_t *d = dst;
	while (n--) *d++ = *src++;
	return dst;
}
