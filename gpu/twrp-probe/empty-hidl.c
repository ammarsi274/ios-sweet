/* Stand-in for android.hardware.graphics.common@1.1.so: Nokia's gralloc.sdm660.so
 * and libgrallocutils.so list it as NEEDED but import no symbol from it
 * (checked with llvm-nm against the vendor_b copies). Our own, empty. */
int drg_empty_hidl_types;
