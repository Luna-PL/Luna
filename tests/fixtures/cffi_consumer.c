#include "cffi.h"

int main(void) {
    if (luna_add(19, 23) != 42) return 1;
    if (luna_scale(2.5, 4.0) != 10.0) return 2;
    return 0;
}
