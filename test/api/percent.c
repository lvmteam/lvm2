#include "lvm2app.h"

#define assert(x) do { if (!(x)) goto bad; } while (0)

int main(int argc, char *argv[])
{
        lvm_t handle = lvm_init(NULL);
        assert(handle);

	vg_t vg = lvm_vg_open(handle, argv[1], "r", 0);
        assert(vg);

        lv_t lv = lvm_lv_from_name(vg, "snap");
        assert(lv);

        struct lvm_property_value v = lvm_lv_get_property(lv, "snap_percent");
        assert(v.is_valid);
        assert(v.value.integer == PERCENT_0);

        lv = lvm_lv_from_name(vg, "mirr");
        assert(lv);

        v = lvm_lv_get_property(lv, "copy_percent");
        assert(v.is_valid);
        assert(v.value.integer == PERCENT_100);

        lv = lvm_lv_from_name(vg, "snap2");
        assert(lv);

        v = lvm_lv_get_property(lv, "snap_percent");
        assert(v.is_valid);
        assert(v.value.integer == 50 * PERCENT_1);

        lvm_vg_close(vg);
        return 0;

bad:
	if (handle && lvm_errno(handle))
		fprintf(stderr, "LVM Error: %s\n", lvm_errmsg(handle));
	if (vg)
		lvm_vg_close(vg);
	if (handle)
		lvm_quit(handle);
	return 1;
}
