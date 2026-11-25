#ifndef PB2_DISPATCH_H
#define PB2_DISPATCH_H

typedef struct DispatchEntry {
    unsigned addr;     /* | 1 for Thumb */
    int overlay;       /* -1: main / ITCM / DTCM / extended main */
    void (*fn)(void);
} DispatchEntry;

extern const DispatchEntry gDispatch[];
extern const unsigned gDispatchCount;

#endif
