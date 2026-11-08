/* Force-included when building firmware sources with MSVC: the alignment
 * attribute in patch.c doesn't matter on the PC. */
#define __attribute__(x)
