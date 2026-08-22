#include "containers.h"
#include <stdlib.h>
#include <assert.h>


/* ======================================================================== */
/* ARWDeque — doubly-linked list of fixed-size blocks                        */
/* ======================================================================== */

static ARWBlock *
arwdeque_newblock(ARWDeque *d)
{
    if (d->numfreeblocks) {
        d->numfreeblocks--;
        return d->freeblocks[d->numfreeblocks];
    }
    return malloc(sizeof(ARWBlock));
}

static void
arwdeque_freeblock(ARWDeque *d, ARWBlock *b)
{
    if (d->numfreeblocks < ARW_MAX_FREEBLOCKS) {
        d->freeblocks[d->numfreeblocks] = b;
        d->numfreeblocks++;
    } else {
        free(b);
    }
}

void
arwdeque_init(ARWDeque *d)
{
    ARWBlock *b = malloc(sizeof(ARWBlock));
    b->leftlink = NULL;
    b->rightlink = NULL;
    d->leftblock = b;
    d->rightblock = b;
    d->leftindex = ARW_BLOCK_CENTER + 1;
    d->rightindex = ARW_BLOCK_CENTER;
    d->len = 0;
    d->numfreeblocks = 0;
}

void
arwdeque_free(ARWDeque *d)
{
    ARWBlock *b = d->leftblock;
    while (b) {
        ARWBlock *next = b->rightlink;
        free(b);
        b = next;
    }
    for (size_t i = 0; i < d->numfreeblocks; i++)
        free(d->freeblocks[i]);
    d->leftblock = NULL;
    d->rightblock = NULL;
    d->len = 0;
    d->numfreeblocks = 0;
}

int
arwdeque_append(ARWDeque *d, ARW value)
{
    if (d->rightindex == ARW_BLOCKLEN - 1) {
        ARWBlock *b = arwdeque_newblock(d);
        if (b == NULL)
            return -1;
        b->leftlink = d->rightblock;
        d->rightblock->rightlink = b;
        d->rightblock = b;
        b->rightlink = NULL;
        d->rightindex = (size_t)-1;
    }
    d->len++;
    d->rightindex++;
    d->rightblock->data[d->rightindex] = value;
    return 0;
}

int
arwdeque_appendleft(ARWDeque *d, ARW value)
{
    if (d->leftindex == 0) {
        ARWBlock *b = arwdeque_newblock(d);
        if (b == NULL)
            return -1;
        b->rightlink = d->leftblock;
        d->leftblock->leftlink = b;
        d->leftblock = b;
        b->leftlink = NULL;
        d->leftindex = ARW_BLOCKLEN;
    }
    d->len++;
    d->leftindex--;
    d->leftblock->data[d->leftindex] = value;
    return 0;
}

int
arwdeque_pop(ARWDeque *d, ARW *out)
{
    if (d->len == 0)
        return -1;

    *out = d->rightblock->data[d->rightindex];
    d->rightindex--;
    d->len--;

    if (d->rightindex == (size_t)-1) {
        if (d->len) {
            ARWBlock *prev = d->rightblock->leftlink;
            arwdeque_freeblock(d, d->rightblock);
            prev->rightlink = NULL;
            d->rightblock = prev;
            d->rightindex = ARW_BLOCKLEN - 1;
        } else {
            d->leftindex = ARW_BLOCK_CENTER + 1;
            d->rightindex = ARW_BLOCK_CENTER;
        }
    }
    return 0;
}

int
arwdeque_popleft(ARWDeque *d, ARW *out)
{
    if (d->len == 0)
        return -1;

    *out = d->leftblock->data[d->leftindex];
    d->leftindex++;
    d->len--;

    if (d->leftindex == ARW_BLOCKLEN) {
        if (d->len) {
            ARWBlock *next = d->leftblock->rightlink;
            arwdeque_freeblock(d, d->leftblock);
            next->leftlink = NULL;
            d->leftblock = next;
            d->leftindex = 0;
        } else {
            d->leftindex = ARW_BLOCK_CENTER + 1;
            d->rightindex = ARW_BLOCK_CENTER;
        }
    }
    return 0;
}

int
arwdeque_get(const ARWDeque *d, size_t index, ARW *out)
{
    if (index >= d->len)
        return -1;

    size_t real = d->leftindex + index;
    const ARWBlock *b = d->leftblock;
    while (real >= ARW_BLOCKLEN) {
        b = b->rightlink;
        real -= ARW_BLOCKLEN;
    }
    *out = b->data[real];
    return 0;
}

size_t
arwdeque_len(const ARWDeque *d)
{
    return d->len;
}

