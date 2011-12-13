#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#define DECL(n) \
	extern CU_TestInfo n ## _list[]; \
	int n ## _init(void); \
	int n ## _fini(void);
#define USE(n) { (char*) #n, n##_init, n##_fini, n##_list }

DECL(bitset);
DECL(regex);
DECL(config);

CU_SuiteInfo suites[] = {
	USE(bitset),
	USE(regex),
	USE(config),
	CU_SUITE_INFO_NULL
};

int main(int argc, char **argv) {
	CU_initialize_registry();
	CU_register_suites(suites);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	return CU_get_number_of_failures() != 0;
}
