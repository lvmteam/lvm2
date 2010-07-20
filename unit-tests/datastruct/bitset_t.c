#include "libdevmapper.h"

#include <assert.h>

enum {
        NR_BITS = 137
};

int main(int argc, char **argv)
{
        int i, j, last, first;
        dm_bitset_t bs;
        struct dm_pool *mem = dm_pool_create("bitset test", 1024);

        assert(mem);
        bs = dm_bitset_create(mem, NR_BITS);

        for (i = 0; i < NR_BITS; i++)
                assert(!dm_bit(bs, i));

        for (i = 0, j = 1; i < NR_BITS; i += j, j++)
                dm_bit_set(bs, i);

        first = 1;
        for (i = 0, j = 1; i < NR_BITS; i += j, j++) {
                if (first) {
                        last = dm_bit_get_first(bs);
                        first = 0;
                } else
                        last = dm_bit_get_next(bs, last);

                assert(last == i);
        }

        assert(dm_bit_get_next(bs, last) == -1);
        dm_pool_destroy(mem);

        return 0;
}

