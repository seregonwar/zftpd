#include "pal_scratch.h"
#include <stdint.h>
 
int main(void)
{
    uint8_t *p1 = NULL;
    uint8_t *p2 = NULL;
 
    if (pal_scratch_acquire(&p1, 16U) != 0) {
        return 1;
    }
    if (p1 == NULL) {
        return 2;
    }
 
    if (pal_scratch_acquire(&p2, 16U) == 0) {
        return 3;
    }
 
    pal_scratch_release(p1);
 
    if (pal_scratch_acquire(&p2, 16U) != 0) {
        return 4;
    }
    if (p2 == NULL) {
        return 5;
    }
    pal_scratch_release(p2);
 
    return 0;
}
