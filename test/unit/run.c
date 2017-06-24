#include "units.h"
#include <CUnit/Basic.h>

#include <stdio.h>
#include <stdlib.h>

/* Setup SuiteInfo struct in a  compatible way across different CUnit versions */
/* old version of CUnit has used char* for .pName, so using cast here */
#define USE(n) { \
	.pName = (char*) #n, \
	.pInitFunc = n##_init, \
	.pCleanupFunc = n##_fini, \
	.pTests = n##_list }

CU_SuiteInfo suites[] = {
	USE(bitset),
	USE(config),
	USE(dmlist),
	USE(dmstatus),
	USE(regex),
	USE(percent),
	USE(string),
	CU_SUITE_INFO_NULL
};

int main(int argc, char **argv) {
	if (CU_initialize_registry() != CUE_SUCCESS) {
		printf("Initialization of Test Registry failed.\n");
		return CU_get_error();
	}

	CU_register_suites(suites);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	CU_cleanup_registry();

	return (CU_get_number_of_failures() != 0);
}
